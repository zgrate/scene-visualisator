#include "protocol/dmx_engine.h"

#include "protocol/packets.h"

#include <asio.hpp>

#include <algorithm>
#include <cstdio>
#include <random>
#include <set>
#include <thread>

namespace sv::proto {

namespace {

using asio::ip::udp;
constexpr auto tick = std::chrono::microseconds(1'000'000 / 44); // internal sources run at DMX rate
const std::string generator_id = "internal:generator", frame_id = "internal:frame", playback_id = "internal:playback",
                  programmer_id = "internal:programmer";

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
    std::atomic<std::int64_t> timecode_ms{-1}, timecode_at{0}; // at: steady_clock ticks
    std::atomic<double> timecode_fps{25};
    std::atomic<std::uint64_t> published{0}; // every universe publish, for the viewport's redraw check
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
        std::optional<PlaybackMix> playback;                          // mixed into the merge while set
        std::optional<std::pair<Slots, std::bitset<512>>> programmer; // overrides its masked slots
        Slots published{};                                            // the last result, for the output
        bool out_dirty = false;
        Clock::time_point out_sent{};
        std::uint8_t out_seq = 0;
    };

    EngineConfig config;
    asio::io_context io;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
    std::thread thread;
    udp::socket artnet{io}, sacn{io}, out{io}, ddp_out{io};
    asio::steady_timer tick_timer{io}, stats_timer{io}, out_timer{io}, ddp_timer{io};
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
    std::function<void(std::uint16_t, const Slots &)> tap;
    OutputConfig out_config;
    udp::endpoint out_self; // where our output packets come from, so they are never taken as input
    Cid cid = [] {
        Cid c;
        std::random_device rd;
        for (auto &b : c)
            b = std::uint8_t(rd());
        return c;
    }();
    std::uint64_t out_packets = 0;
    std::set<std::uint16_t> out_universes;
    std::vector<DdpTarget> ddp_targets;
    std::uint8_t ddp_seq = 0;
    std::uint64_t ddp_packets = 0;

    std::mutex ddp_mutex;
    std::map<std::string, std::vector<std::uint8_t>> ddp_pixels; // set_ddp_pixels, by target key

    mutable std::mutex bus_mutex; // guards the map, not the buffers
    std::map<std::uint16_t, std::unique_ptr<TripleBuffer<UniverseFrame>>> bus;
    mutable std::mutex status_mutex;
    std::map<std::uint16_t, UniverseStatus> status;
    OutputStatus out_status;

    TripleBuffer<UniverseFrame> &buffer(std::uint16_t u) {
        std::lock_guard lock(bus_mutex);
        auto &b = bus[u];
        if (!b)
            b = std::make_unique<TripleBuffer<UniverseFrame>>();
        return *b;
    }

    void publish(std::uint16_t u, Universe &uni) {
        auto merged = uni.merger.merge();
        auto next_index = std::uint8_t(uni.merger.sources().size());
        if (uni.playback) {
            std::bitset<512> recorded;
            merged.data = mix_playback(merged.data, *uni.playback, &recorded);
            for (std::size_t i = 0; i < 512; ++i)
                if (recorded[i])
                    merged.winner[i] = next_index;
            ++next_index;
        }
        const auto programmer_index = next_index;
        if (uni.programmer)
            for (std::size_t i = 0; i < 512; ++i)
                if (uni.programmer->second[i])
                    merged.data[i] = uni.programmer->first[i], merged.winner[i] = programmer_index;
        auto &tb = buffer(u);
        tb.back() = {merged.data, ++uni.serial};
        tb.publish();
        ++published;
        uni.published = merged.data, uni.out_dirty = true;
        if (tap)
            tap(u, merged.data);
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
        if (uni.playback)
            st.sources.push_back("Playback"), st.source_ids.push_back(playback_id);
        if (uni.programmer)
            st.sources.push_back("Programmer"), st.source_ids.push_back(programmer_id);
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

    // Our own output coming back (broadcast, multicast loop or a unicast to ourselves) would freeze the merge on
    // itself.
    bool own(const udp::endpoint &from) const {
        return out.is_open() && from.port() == out_self.port() &&
               (from.address() == out_self.address() || from.address().is_loopback());
    }

    void on_artnet(std::span<const std::uint8_t> p, const udp::endpoint &from) {
        if (own(from))
            return;
        const auto packet = parse_artnet(p);
        const std::string ip = from.address().to_string();
        if (std::holds_alternative<ArtPoll>(packet))
            return reply_poll(from);
        if (const auto *tc = std::get_if<ArtTimeCode>(&packet)) {
            timecode_ms = tc->ms(), timecode_fps = tc->fps();
            timecode_at = Clock::now().time_since_epoch().count();
            return;
        }
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
        if (own(from))
            return;
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

    void apply_output(OutputConfig c) {
        std::error_code ec;
        out_timer.cancel();
        out.close(ec);
        out_config = std::move(c);
        std::string error;
        if (out_config.enabled) {
            const std::uint16_t port = out_config.port ? out_config.port : out_config.sacn ? sacn_port : artnet_port;
            try {
                const auto probe_to =
                    out_config.target.empty()
                        ? udp::endpoint(out_config.sacn
                                            ? asio::ip::address(asio::ip::address_v4(sacn_multicast_group(1)))
                                            : asio::ip::address(asio::ip::address_v4::broadcast()),
                                        port)
                        : udp::endpoint(asio::ip::make_address_v4(out_config.target), port);
                out.open(udp::v4());
                out.set_option(asio::socket_base::broadcast(true));
                if (out_config.sacn && config.sacn_interface != "0.0.0.0")
                    out.set_option(
                        asio::ip::multicast::outbound_interface(asio::ip::make_address_v4(config.sacn_interface)));
                out.bind(udp::endpoint(asio::ip::make_address(config.bind_address), 0)); // ephemeral: see own()
                out_self = out.local_endpoint();
                if (out_self.address().is_unspecified()) { // the local address the kernel sends from
                    udp::socket probe(io, udp::v4());
                    probe.set_option(asio::socket_base::broadcast(true));
                    probe.connect(probe_to, ec);
                    if (!ec)
                        out_self.address(probe.local_endpoint().address());
                }
                for (auto &[_, uni] : universes)
                    uni.out_dirty = uni.serial > 0;
                send_output();
            } catch (const std::system_error &e) {
                out.close(ec);
                error = e.what();
            }
        }
        std::lock_guard lock(status_mutex);
        out_status = {out_config.enabled, 0, 0, error};
    }

    void send_output() {
        if (!out.is_open())
            return;
        const auto now = Clock::now();
        const std::uint16_t port = out_config.port ? out_config.port : out_config.sacn ? sacn_port : artnet_port;
        std::error_code ec, last;
        for (auto &[u, uni] : universes) {
            if (uni.serial == 0 || (!uni.out_dirty && now - uni.out_sent < std::chrono::seconds(1)))
                continue; // nothing published yet, or unchanged and refreshed within the second
            const int target_u = u + out_config.universe_offset;
            if (target_u < 1 || target_u > (out_config.sacn ? 63999 : 32768))
                continue;
            const auto tu = std::uint16_t(target_u);
            Bytes packet;
            asio::ip::address_v4 to;
            if (out_config.sacn) {
                packet = make_sacn_data(cid, config.node_name, out_config.priority, uni.out_seq++, tu, uni.published);
                to = out_config.target.empty() ? asio::ip::address_v4(sacn_multicast_group(tu))
                                               : asio::ip::make_address_v4(out_config.target, ec);
            } else {
                uni.out_seq = std::uint8_t(uni.out_seq % 255 + 1); // 1..255, 0 would switch sequencing off
                packet = make_artdmx(port_address_from_universe(tu), uni.out_seq, uni.published);
                to = out_config.target.empty() ? asio::ip::address_v4::broadcast()
                                               : asio::ip::make_address_v4(out_config.target, ec);
            }
            out.send_to(asio::buffer(packet), udp::endpoint(to, port), 0, ec);
            if (ec)
                last = ec;
            else
                ++out_packets, out_universes.insert(u);
            uni.out_dirty = false, uni.out_sent = now;
        }
        if (last) {
            std::lock_guard lock(status_mutex);
            out_status.error = last.message();
        }
        out_timer.expires_after(
            std::chrono::microseconds(std::int64_t(1e6 / std::clamp(out_config.rate_hz, 1.0, 100.0))));
        out_timer.async_wait([this](std::error_code e) {
            if (!e)
                send_output();
        });
    }

    void apply_ddp(std::vector<DdpTarget> targets) {
        std::error_code ec;
        ddp_timer.cancel();
        ddp_targets = std::move(targets);
        std::string error;
        if (ddp_targets.empty()) {
            ddp_out.close(ec);
        } else if (!ddp_out.is_open()) {
            try {
                ddp_out.open(udp::v4());
                ddp_out.set_option(asio::socket_base::broadcast(true));
                ddp_out.bind(udp::endpoint(asio::ip::make_address(config.bind_address), 0));
            } catch (const std::system_error &e) {
                ddp_out.close(ec);
                error = e.what();
            }
        }
        {
            std::lock_guard lock(status_mutex);
            out_status.ddp_targets = ddp_targets.size(), out_status.ddp_error = error;
        }
        {
            std::lock_guard lock(ddp_mutex);
            std::erase_if(ddp_pixels, [&](auto &kv) {
                return std::ranges::find(ddp_targets, kv.first, &DdpTarget::key) == ddp_targets.end();
            });
        }
        send_ddp();
    }

    // One frame to every target. Targets sharing a controller go out in offset order and only the controller's last
    // packet carries PUSH, so its whole buffer shows at once.
    void send_ddp() {
        if (!ddp_out.is_open())
            return;
        struct Part {
            std::uint32_t offset;
            bool rgbw;
            std::vector<std::uint8_t> bytes;
        };
        std::map<udp::endpoint, std::vector<Part>> frames;
        std::string error;
        for (const DdpTarget &t : ddp_targets) {
            std::error_code ec;
            const auto ip = asio::ip::make_address_v4(t.ip, ec);
            if (ec) {
                error = "Bad DDP target \"" + t.ip + "\"";
                continue;
            }
            std::vector<std::uint8_t> bytes;
            if (t.channels.empty()) {
                std::lock_guard lock(ddp_mutex);
                if (auto it = ddp_pixels.find(t.key); it != ddp_pixels.end())
                    bytes = it->second;
            } else {
                bytes.reserve(t.channels.size());
                for (auto [u, slot] : t.channels) {
                    auto it = universes.find(u);
                    bytes.push_back(slot >= 1 && slot <= 512 && it != universes.end() ? it->second.published[slot - 1]
                                                                                      : 0);
                }
            }
            if (!bytes.empty())
                frames[udp::endpoint(ip, t.port)].push_back({t.offset, t.rgbw, std::move(bytes)});
        }
        ddp_seq = std::uint8_t(ddp_seq % 15 + 1); // 1..15, 0 means no sequence
        for (auto &[to, parts] : frames) {
            std::ranges::sort(parts, {}, &Part::offset);
            for (std::size_t p = 0; p < parts.size(); ++p) {
                const auto &[offset, rgbw, bytes] = parts[p];
                for (std::size_t at = 0; at < bytes.size(); at += ddp_max_data) {
                    const std::size_t n = std::min(ddp_max_data, bytes.size() - at);
                    const bool push = p + 1 == parts.size() && at + n == bytes.size();
                    std::error_code ec;
                    ddp_out.send_to(asio::buffer(make_ddp(ddp_seq, std::uint32_t(offset + at),
                                                          std::span(bytes).subspan(at, n), push, rgbw)),
                                    to, 0, ec);
                    if (ec)
                        error = ec.message();
                    else
                        ++ddp_packets;
                }
            }
        }
        {
            std::lock_guard lock(status_mutex);
            out_status.ddp_error = error;
        }
        ddp_timer.expires_after(
            std::chrono::microseconds(std::int64_t(1e6 / std::clamp(out_config.rate_hz, 1.0, 100.0))));
        ddp_timer.async_wait([this](std::error_code e) {
            if (!e)
                send_ddp();
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
                auto &src = uni.merger.sources(); // Apple Clang 15 cannot capture the structured binding `uni`
                std::erase_if(uni.last_sequence,
                              [&](auto &kv) { return std::ranges::find(src, kv.first, &Source::id) == src.end(); });
                if (uni.merger.expire(now)) {
                    publish(u, uni);
                } else {
                    std::lock_guard lock(status_mutex);
                    status[u].rate_hz = uni.rate_hz, status[u].lost = uni.lost, status[u].dropped = uni.dropped;
                }
            }
            {
                std::lock_guard lock(status_mutex);
                out_status.packets_per_s = double(out_packets), out_status.universes = out_universes.size();
                out_status.ddp_packets_per_s = double(ddp_packets);
            }
            out_packets = 0, out_universes.clear(), ddp_packets = 0;
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
    if (m.out_config.enabled) // reopened after a stop()
        asio::post(m.io, [&m] { m.apply_output(m.out_config); });
    if (!m.ddp_targets.empty())
        asio::post(m.io, [&m] { m.apply_ddp(m.ddp_targets); });
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
    m.out.close(ec);
    m.ddp_out.close(ec);
    m.ddp_timer.cancel();
    m.tick_timer.cancel();
    m.stats_timer.cancel();
    m.out_timer.cancel();
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

void DmxEngine::set_playback(std::uint16_t universe, std::optional<PlaybackMix> mix) {
    asio::post(impl_->io, [&m = *impl_, universe, mix = std::move(mix)]() mutable {
        if (auto it = m.universes.find(universe); it != m.universes.end()) {
            it->second.playback = std::move(mix);
            m.publish(universe, it->second);
        }
    });
}

void DmxEngine::set_output(OutputConfig config) {
    asio::post(impl_->io, [&m = *impl_, config = std::move(config)]() mutable { m.apply_output(std::move(config)); });
}

void DmxEngine::set_ddp(std::vector<DdpTarget> targets) {
    asio::post(impl_->io, [&m = *impl_, targets = std::move(targets)]() mutable { m.apply_ddp(std::move(targets)); });
}

void DmxEngine::set_ddp_pixels(const std::string &key, std::vector<std::uint8_t> pixels) {
    std::lock_guard lock(impl_->ddp_mutex);
    impl_->ddp_pixels[key] = std::move(pixels);
}

std::uint64_t DmxEngine::publish_serial() const { return impl_->published; }

Timecode DmxEngine::timecode() const {
    const auto age = Clock::now() - Clock::time_point(Clock::duration(impl_->timecode_at.load()));
    if (impl_->timecode_ms < 0 || age > std::chrono::seconds(1))
        return {};
    return {impl_->timecode_ms, impl_->timecode_fps};
}

OutputStatus DmxEngine::output_status() const {
    std::lock_guard lock(impl_->status_mutex);
    return impl_->out_status;
}

void DmxEngine::set_programmer(std::uint16_t universe, std::optional<std::pair<Slots, std::bitset<512>>> programmer) {
    asio::post(impl_->io, [&m = *impl_, universe, programmer] {
        if (auto it = m.universes.find(universe); it != m.universes.end()) {
            it->second.programmer = programmer;
            m.publish(universe, it->second);
        }
    });
}

void DmxEngine::set_tap(std::function<void(std::uint16_t, const Slots &)> tap) {
    asio::post(impl_->io, [&m = *impl_, tap = std::move(tap)]() mutable { m.tap = std::move(tap); });
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
