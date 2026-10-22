#include "protocol/dmx_engine.h"

#include "protocol/packets.h"

#include <asio.hpp>

#include <algorithm>
#include <cstdio>
#include <set>
#include <thread>

namespace sv::proto {

namespace {

using asio::ip::udp;
constexpr auto tick = std::chrono::microseconds(1'000'000 / 44); // internal sources run at DMX rate
const std::string generator_id = "internal:generator", frame_id = "internal:frame";

std::string cid_hex(const Cid &cid) {
    std::string s;
    char b[3];
    for (auto c : cid)
        std::snprintf(b, sizeof b, "%02x", c), s += b;
    return s;
}

// True if `seq` is older than `last` and must be dropped (spec "Protocol specification").
bool artnet_stale(std::uint8_t last, std::uint8_t seq) { return seq != 0 && last != 0 && std::int8_t(seq - last) < 0; }
bool sacn_stale(std::uint8_t last, std::uint8_t seq) {
    const int diff = std::int8_t(seq - last);
    return diff <= 0 && diff > -20;
}

} // namespace

struct DmxEngine::Impl {
    struct Pending {
        std::string id, name;
        Slots data;
        std::uint16_t count;
        std::uint8_t priority;
        Clock::duration timeout;
    };
    struct Universe {
        UniverseMerger merger;
        std::map<std::string, std::uint8_t> last_sequence;
        std::vector<Pending> artsync_pending;
        std::map<std::uint16_t, std::vector<Pending>> sacn_pending; // by sync address
        std::uint64_t frames_this_second = 0, lost = 0, dropped = 0;
        double rate_hz = 0;
        std::uint64_t serial = 0;
    };

    EngineConfig config;
    asio::io_context io;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
    std::thread thread;
    udp::socket artnet{io}, sacn{io};
    asio::steady_timer tick_timer{io}, stats_timer{io};
    std::array<std::uint8_t, 1500> artnet_buf{}, sacn_buf{};
    udp::endpoint artnet_from, sacn_from;
    Clock::time_point last_artsync{};
    Clock::time_point started = Clock::now();

    // network thread only
    std::map<std::uint16_t, Universe> universes;
    std::set<std::uint16_t> joined;
    Pattern pattern = Pattern::off;
    double speed = 1;
    std::vector<GeneratorTarget> targets;

    mutable std::mutex bus_mutex; // guards the map, not the buffers
    std::map<std::uint16_t, std::unique_ptr<TripleBuffer<UniverseFrame>>> bus;
    mutable std::mutex status_mutex;
    std::map<std::uint16_t, UniverseStatus> status;

    TripleBuffer<UniverseFrame> &buffer(std::uint16_t u) {
        std::lock_guard lock(bus_mutex);
        auto &b = bus[u];
        if (!b)
            b = std::make_unique<TripleBuffer<UniverseFrame>>();
        return *b;
    }

    void publish(std::uint16_t u, Universe &uni) {
        const auto merged = uni.merger.merge();
        auto &tb = buffer(u);
        tb.back() = {merged.data, ++uni.serial};
        tb.publish();
        ++uni.frames_this_second;
        std::lock_guard lock(status_mutex);
        UniverseStatus &st = status[u];
        st.universe = u;
        st.data = merged.data;
        st.winner = merged.winner;
        st.policy = uni.merger.policy();
        st.sources.clear(), st.source_ids.clear();
        for (auto &s : uni.merger.sources())
            st.sources.push_back(s.name), st.source_ids.push_back(s.id);
        st.rate_hz = uni.rate_hz, st.lost = uni.lost, st.dropped = uni.dropped;
    }

    void ingest(std::uint16_t u, Universe &uni, const Pending &p) {
        uni.merger.receive(p.id, p.name, std::span(p.data.data(), p.count), p.priority, Clock::now(), p.timeout);
        publish(u, uni);
    }

    // Sequence bookkeeping shared by both protocols; returns false if the packet must be dropped.
    bool accept_sequence(Universe &uni, const std::string &id, std::uint8_t seq, bool sacn_rule) {
        auto [it, fresh] = uni.last_sequence.try_emplace(id, seq);
        if (fresh)
            return true;
        if (sacn_rule ? sacn_stale(it->second, seq) : artnet_stale(it->second, seq)) {
            ++uni.dropped;
            return false;
        }
        if (seq != 0 && it->second != 0) {
            const std::uint8_t gap = std::uint8_t(seq - it->second - 1);
            if (gap < 128)
                uni.lost += gap;
        }
        it->second = seq;
        return true;
    }

    void on_artnet(std::span<const std::uint8_t> p, const udp::endpoint &from) {
        const auto packet = parse_artnet(p);
        const std::string ip = from.address().to_string();
        if (std::holds_alternative<ArtPoll>(packet))
            return reply_poll(from);
        if (std::holds_alternative<ArtSync>(packet)) {
            // shortcut: ArtSync from any IP releases held frames; Art-Net 4 says only the ArtDmx sender's counts.
            last_artsync = Clock::now();
            for (auto &[u, uni] : universes) {
                for (auto &pend : uni.artsync_pending)
                    uni.merger.receive(pend.id, pend.name, std::span(pend.data.data(), pend.count), pend.priority,
                                       last_artsync, pend.timeout);
                if (!uni.artsync_pending.empty())
                    publish(u, uni);
                uni.artsync_pending.clear();
            }
            return;
        }
        auto *dmx = std::get_if<ArtDmx>(&packet);
        if (!dmx)
            return;
        auto it = universes.find(universe_from_port_address(dmx->port_address));
        if (it == universes.end())
            return;
        Universe &uni = it->second;
        const std::string id = "artnet:" + ip;
        if (!accept_sequence(uni, id, dmx->sequence, false))
            return;
        Pending pend{id, "Art-Net " + ip, {}, std::uint16_t(dmx->slots.size()), 100, artnet_timeout};
        std::ranges::copy(dmx->slots, pend.data.begin());
        if (Clock::now() - last_artsync < artsync_timeout) {
            std::erase_if(uni.artsync_pending, [&](auto &q) { return q.id == id; });
            uni.artsync_pending.push_back(pend);
            return;
        }
        ingest(it->first, uni, pend);
    }

    void on_sacn(std::span<const std::uint8_t> p, const udp::endpoint &from) {
        const auto packet = parse_sacn(p);
        if (auto *sync = std::get_if<SacnSync>(&packet)) {
            for (auto &[u, uni] : universes) {
                auto pending = uni.sacn_pending.extract(sync->sync_address);
                if (pending.empty())
                    continue;
                for (auto &pend : pending.mapped())
                    uni.merger.receive(pend.id, pend.name, std::span(pend.data.data(), pend.count), pend.priority,
                                       Clock::now(), pend.timeout);
                publish(u, uni);
            }
            return;
        }
        auto *d = std::get_if<SacnData>(&packet);
        if (!d)
            return;
        auto it = universes.find(d->universe);
        if (it == universes.end())
            return;
        Universe &uni = it->second;
        const std::string id = "sacn:" + cid_hex(d->cid);
        if (d->terminated) {
            uni.last_sequence.erase(id);
            if (uni.merger.remove(id))
                publish(it->first, uni);
            return;
        }
        if (d->start_code == 0xDD) {
            uni.merger.receive_slot_priority(id, d->slots, Clock::now());
            return;
        }
        if (d->start_code != 0 || !accept_sequence(uni, id, d->sequence, true))
            return;
        Pending pend{id,          d->source_name + " (" + from.address().to_string() + ")",
                     {},          std::uint16_t(d->slots.size()),
                     d->priority, sacn_timeout};
        std::ranges::copy(d->slots, pend.data.begin());
        if (d->sync_address != 0) {
            auto &q = uni.sacn_pending[d->sync_address];
            std::erase_if(q, [&](auto &x) { return x.id == id; });
            q.push_back(pend);
            return;
        }
        ingest(it->first, uni, pend);
    }

    void reply_poll(const udp::endpoint &to) {
        std::array<std::uint8_t, 4> ip{};
        try { // the local address that routes to the poller
            udp::socket probe(io, udp::v4());
            probe.connect(to);
            ip = probe.local_endpoint().address().to_v4().to_bytes();
        } catch (const std::system_error &) {
        }
        // One reply per group of up to 4 universes sharing Net:Sub-Net (Art-Net 4 BindIndex).
        std::map<std::uint16_t, std::vector<std::uint16_t>> groups;
        for (auto &[u, _] : universes) {
            const std::uint16_t pa = port_address_from_universe(u);
            auto &g = groups[pa & 0x7FF0];
            if (g.size() == 4)
                groups[0x8000 | pa].push_back(pa); // overflow group, still unique key
            else
                g.push_back(pa);
        }
        std::uint8_t bind = 1;
        for (auto &[_, pas] : groups) {
            auto reply = std::make_shared<Bytes>(make_artpollreply(ip, "SceneVis", config.node_name, pas, bind++));
            artnet.async_send_to(asio::buffer(*reply), to, [reply](auto, auto) {});
        }
        if (groups.empty()) {
            auto reply = std::make_shared<Bytes>(make_artpollreply(ip, "SceneVis", config.node_name, {}, 1));
            artnet.async_send_to(asio::buffer(*reply), to, [reply](auto, auto) {});
        }
    }

    void receive_artnet() {
        artnet.async_receive_from(asio::buffer(artnet_buf), artnet_from, [this](std::error_code ec, std::size_t n) {
            if (ec == asio::error::operation_aborted)
                return;
            if (!ec)
                on_artnet(std::span(artnet_buf.data(), n), artnet_from);
            receive_artnet();
        });
    }

    void receive_sacn() {
        sacn.async_receive_from(asio::buffer(sacn_buf), sacn_from, [this](std::error_code ec, std::size_t n) {
            if (ec == asio::error::operation_aborted)
                return;
            if (!ec)
                on_sacn(std::span(sacn_buf.data(), n), sacn_from);
            receive_sacn();
        });
    }

    void run_tick() {
        tick_timer.expires_after(tick);
        tick_timer.async_wait([this](std::error_code ec) {
            if (ec)
                return;
            if (pattern != Pattern::off) {
                std::map<std::uint16_t, Slots> frames;
                const double t = std::chrono::duration<double>(Clock::now() - started).count();
                run_generator(pattern, t, speed, targets, frames);
                for (auto &[u, data] : frames)
                    if (auto it = universes.find(u); it != universes.end())
                        ingest(u, it->second, {generator_id, "Generator", data, 512, 100, Clock::duration::max()});
            }
            run_tick();
        });
    }

    void run_stats() {
        stats_timer.expires_after(std::chrono::seconds(1));
        stats_timer.async_wait([this](std::error_code ec) {
            if (ec)
                return;
            const auto now = Clock::now();
            for (auto &[u, uni] : universes) {
                uni.rate_hz = double(uni.frames_this_second);
                uni.frames_this_second = 0;
                std::erase_if(uni.last_sequence, [&](auto &kv) {
                    auto &src = uni.merger.sources();
                    return std::ranges::find(src, kv.first, &Source::id) == src.end();
                });
                if (uni.merger.expire(now)) {
                    publish(u, uni);
                } else {
                    std::lock_guard lock(status_mutex);
                    status[u].rate_hz = uni.rate_hz, status[u].lost = uni.lost, status[u].dropped = uni.dropped;
                }
            }
            run_stats();
        });
    }

    void join_groups() {
        if (!sacn.is_open())
            return;
        std::set<std::uint16_t> want;
        for (auto &[u, _] : universes)
            want.insert(u);
        const auto iface = asio::ip::make_address_v4(config.sacn_interface);
        for (std::uint16_t u : joined)
            if (!want.contains(u)) {
                std::error_code ec;
                sacn.set_option(asio::ip::multicast::leave_group(asio::ip::address_v4(sacn_multicast_group(u)), iface),
                                ec);
            }
        for (std::uint16_t u : want)
            if (!joined.contains(u)) {
                std::error_code ec; // no multicast route (e.g. offline laptop): unicast sACN still works
                sacn.set_option(asio::ip::multicast::join_group(asio::ip::address_v4(sacn_multicast_group(u)), iface),
                                ec);
            }
        joined = std::move(want);
    }

    void open(udp::socket &s, std::uint16_t port) {
        s.open(udp::v4());
        s.set_option(udp::socket::reuse_address(true));
        s.set_option(asio::socket_base::broadcast(true));
        s.bind(udp::endpoint(asio::ip::make_address(config.bind_address), port));
    }
};

DmxEngine::DmxEngine(EngineConfig config) : impl_(std::make_unique<Impl>()) { impl_->config = std::move(config); }

DmxEngine::~DmxEngine() { stop(); }

void DmxEngine::start() {
    Impl &m = *impl_;
    if (m.thread.joinable())
        return;
    if (m.config.artnet) {
        m.open(m.artnet, m.config.artnet_port);
        artnet_bound_ = m.artnet.local_endpoint().port();
        m.receive_artnet();
    }
    if (m.config.sacn) {
        m.open(m.sacn, m.config.sacn_port);
        sacn_bound_ = m.sacn.local_endpoint().port();
        m.join_groups();
        m.receive_sacn();
    }
    m.run_tick();
    m.run_stats();
    m.io.restart();
    m.work.emplace(m.io.get_executor());
    m.thread = std::thread([&m] { m.io.run(); });
}

void DmxEngine::stop() {
    Impl &m = *impl_;
    if (!m.thread.joinable())
        return;
    m.work.reset();
    m.io.stop();
    m.thread.join();
    std::error_code ec;
    m.artnet.close(ec);
    m.sacn.close(ec);
    m.tick_timer.cancel();
    m.stats_timer.cancel();
}

void DmxEngine::subscribe(std::vector<std::uint16_t> list) {
    std::ranges::sort(list);
    list.erase(std::unique(list.begin(), list.end()), list.end());
    std::erase_if(list, [](auto u) { return u < 1 || u > 63999; });
    if (list.size() > max_universes)
        list.resize(max_universes);
    asio::post(impl_->io, [&m = *impl_, list] {
        std::erase_if(m.universes, [&](auto &kv) { return !std::ranges::binary_search(list, kv.first); });
        for (auto u : list)
            m.universes.try_emplace(u);
        {
            std::lock_guard lock(m.status_mutex);
            std::erase_if(m.status, [&](auto &kv) { return !m.universes.contains(kv.first); });
            for (auto u : list)
                m.status.try_emplace(u, UniverseStatus{.universe = u});
        }
        m.join_groups();
    });
}

void DmxEngine::set_policy(std::uint16_t universe, MergePolicy policy) {
    asio::post(impl_->io, [&m = *impl_, universe, policy] {
        if (auto it = m.universes.find(universe); it != m.universes.end()) {
            it->second.merger.set_policy(policy);
            m.publish(universe, it->second);
        }
    });
}

void DmxEngine::set_intensity_mask(std::uint16_t universe, std::bitset<512> mask) {
    asio::post(impl_->io, [&m = *impl_, universe, mask] {
        if (auto it = m.universes.find(universe); it != m.universes.end())
            it->second.merger.set_intensity_mask(mask);
    });
}

void DmxEngine::set_internal_frame(std::uint16_t universe, std::optional<Slots> frame) {
    asio::post(impl_->io, [&m = *impl_, universe, frame] {
        auto it = m.universes.find(universe);
        if (it == m.universes.end())
            return;
        if (frame)
            m.ingest(universe, it->second, {frame_id, "Internal frame", *frame, 512, 100, Clock::duration::max()});
        else if (it->second.merger.remove(frame_id))
            m.publish(universe, it->second);
    });
}

void DmxEngine::set_generator(Pattern pattern, double speed, std::vector<GeneratorTarget> targets) {
    asio::post(impl_->io, [&m = *impl_, pattern, speed, targets = std::move(targets)]() mutable {
        m.pattern = pattern, m.speed = speed, m.targets = std::move(targets);
        if (pattern == Pattern::off)
            for (auto &[u, uni] : m.universes)
                if (uni.merger.remove(generator_id))
                    m.publish(u, uni);
    });
}

TripleBuffer<UniverseFrame> *DmxEngine::frames(std::uint16_t universe) { return &impl_->buffer(universe); }

std::vector<UniverseStatus> DmxEngine::status() const {
    std::lock_guard lock(impl_->status_mutex);
    std::vector<UniverseStatus> out;
    for (auto &[_, s] : impl_->status)
        out.push_back(s);
    return out;
}

} // namespace sv::proto
