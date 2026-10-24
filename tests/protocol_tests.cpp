#include "protocol/constants.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Art-Net Port-Address packs Net:Sub-Net:Universe into 15 bits") {
    CHECK(sv::proto::port_address(0, 0, 1) == 1);
    CHECK(sv::proto::port_address(1, 2, 3) == 0x123);
    CHECK(sv::proto::port_address(0x7F, 0xF, 0xF) == 0x7FFF);
}

TEST_CASE("sACN multicast group is 239.255.U_hi.U_lo") {
    CHECK(sv::proto::sacn_multicast_group(1) == std::array<std::uint8_t, 4>{239, 255, 0, 1});
    CHECK(sv::proto::sacn_multicast_group(0x1234) == std::array<std::uint8_t, 4>{239, 255, 0x12, 0x34});
}

#include "protocol/dmx_engine.h"
#include "protocol/packets.h"

#include <asio.hpp>

#include <atomic>
#include <thread>

using namespace sv::proto;
using namespace std::chrono_literals;

namespace {

const Cid cid_a{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const Cid cid_b{9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};

std::vector<std::uint8_t> frame_of(std::size_t n, std::uint8_t v) { return std::vector<std::uint8_t>(n, v); }

} // namespace

TEST_CASE("ArtDmx builds and parses back") {
    const auto data = frame_of(24, 7);
    const auto b = make_artdmx(0x123, 42, data);
    auto p = parse_artnet(b);
    auto *dmx = std::get_if<ArtDmx>(&p);
    REQUIRE(dmx);
    CHECK(dmx->port_address == 0x123);
    CHECK(dmx->sequence == 42);
    CHECK(dmx->slots.size() == 24);
    CHECK(dmx->slots[23] == 7);
    CHECK(std::holds_alternative<ArtSync>(parse_artnet(make_artsync())));
    CHECK(std::holds_alternative<ArtPoll>(parse_artnet(make_artpoll())));
    const auto tc = parse_artnet(make_arttimecode({12, 10, 0, 1, 1}));
    REQUIRE(std::holds_alternative<ArtTimeCode>(tc));
    CHECK(std::get<ArtTimeCode>(tc).ms() == 3'610'480); // 01:00:10:12 at 25 fps
    auto bad = make_arttimecode({30, 0, 0, 0, 1});      // frame 30 is no timecode
    CHECK(std::holds_alternative<std::monostate>(parse_artnet(bad)));
}

TEST_CASE("Art-Net parser rejects junk and clamps lengths to the packet") {
    CHECK(std::holds_alternative<std::monostate>(parse_artnet(frame_of(30, 0))));
    auto b = make_artdmx(0, 1, frame_of(10, 1));
    b[16] = 0x02, b[17] = 0x00; // claims 512 slots, carries 10
    auto p = parse_artnet(b);
    REQUIRE(std::get_if<ArtDmx>(&p));
    CHECK(std::get<ArtDmx>(p).slots.size() == 10);
    b.resize(17);
    CHECK(std::holds_alternative<std::monostate>(parse_artnet(b)));
}

TEST_CASE("ArtPollReply carries the node and its ports") {
    const std::uint16_t pas[] = {0x0010, 0x0011};
    const auto b = make_artpollreply({10, 0, 0, 5}, "SV", "Scene Visualiser", pas, 2);
    REQUIRE(b.size() == 239);
    CHECK(b[8] == 0x00);
    CHECK(b[9] == 0x21);
    CHECK(b[10] == 10);
    CHECK(b[19] == 0x1); // SubSwitch
    CHECK(b[173] == 2);
    CHECK(b[190] == 0);
    CHECK(b[191] == 1);
    CHECK(b[211] == 2);
    CHECK(std::string(reinterpret_cast<const char *>(&b[44])) == "Scene Visualiser");
}

TEST_CASE("sACN data and sync packets build and parse back") {
    const auto data = frame_of(512, 200);
    const auto b = make_sacn_data(cid_a, "Desk", 150, 9, 7, data, 0, 3, false);
    REQUIRE(b.size() == 638);
    auto p = parse_sacn(b);
    auto *d = std::get_if<SacnData>(&p);
    REQUIRE(d);
    CHECK(d->cid == cid_a);
    CHECK(d->source_name == "Desk");
    CHECK(d->priority == 150);
    CHECK(d->sequence == 9);
    CHECK(d->universe == 7);
    CHECK(d->sync_address == 3);
    CHECK(d->slots.size() == 512);
    CHECK_FALSE(d->terminated);
    auto s = parse_sacn(make_sacn_sync(cid_a, 4, 3));
    REQUIRE(std::get_if<SacnSync>(&s));
    CHECK(std::get<SacnSync>(s).sync_address == 3);
}

TEST_CASE("sACN parser rejects bad universes and truncated property counts") {
    auto b = make_sacn_data(cid_a, "x", 100, 0, 0, frame_of(4, 1));
    CHECK(std::holds_alternative<std::monostate>(parse_sacn(b))); // universe 0 is invalid
    b = make_sacn_data(cid_a, "x", 100, 0, 1, frame_of(4, 1));
    b.resize(b.size() - 2);
    CHECK(std::holds_alternative<std::monostate>(parse_sacn(b)));
}

TEST_CASE("Merger: highest priority wins, ties merge HTP on intensity and LTP elsewhere") {
    UniverseMerger m;
    std::bitset<512> intensity;
    intensity.set(0);
    m.set_intensity_mask(intensity);
    const auto t = Clock::now();
    m.receive("a", "A", std::vector<std::uint8_t>{200, 10}, 100, t, 1s);
    m.receive("b", "B", std::vector<std::uint8_t>{50, 90}, 100, t, 1s);
    auto r = m.merge();
    CHECK(r.data[0] == 200); // HTP
    CHECK(r.data[1] == 90);  // LTP: b arrived last
    CHECK(r.winner[0] == 0);
    CHECK(r.winner[1] == 1);
    m.receive("c", "C", std::vector<std::uint8_t>{1, 1}, 120, t, 1s);
    r = m.merge();
    CHECK(r.data[0] == 1);
    CHECK(r.winner[0] == 2);
}

TEST_CASE("Merger: per-address priority 0 removes a source from a slot") {
    UniverseMerger m;
    const auto t = Clock::now();
    m.receive("a", "A", std::vector<std::uint8_t>{10, 10}, 100, t, 1s);
    m.receive("b", "B", std::vector<std::uint8_t>{90, 90}, 100, t, 1s);
    m.receive_slot_priority("b", std::vector<std::uint8_t>{0, 200}, t);
    const auto r = m.merge();
    CHECK(r.data[0] == 10);
    CHECK(r.data[1] == 90);
}

TEST_CASE("Merger: HTP and LTP policies ignore priority; stale sources expire") {
    UniverseMerger m(MergePolicy::htp);
    const auto t = Clock::now();
    m.receive("a", "A", std::vector<std::uint8_t>{10}, 200, t, 1s);
    m.receive("b", "B", std::vector<std::uint8_t>{90}, 1, t, 1s);
    CHECK(m.merge().data[0] == 90);
    m.set_policy(MergePolicy::ltp);
    m.receive("a", "A", std::vector<std::uint8_t>{10}, 1, t, 1s);
    CHECK(m.merge().data[0] == 10);
    CHECK_FALSE(m.expire(t + 500ms));
    CHECK(m.expire(t + 2s));
    CHECK(m.sources().empty());
}

TEST_CASE("TripleBuffer hands the consumer the latest published value") {
    TripleBuffer<int> tb;
    CHECK_FALSE(tb.update());
    tb.back() = 1, tb.publish();
    tb.back() = 2, tb.publish();
    CHECK(tb.update());
    CHECK(tb.front() == 2);
    CHECK_FALSE(tb.update());
    tb.back() = 3, tb.publish();
    CHECK(tb.update());
    CHECK(tb.front() == 3);
}

TEST_CASE("Generators: full puts pan/tilt at centre, pixel cells get colour") {
    GeneratorTarget f{.universe = 2, .address = 10, .channels = {{"Dimmer", 0}, {"Pan", 1}, {"Tilt", 2}}};
    f.cells = {{3, 4, 5}};
    std::map<std::uint16_t, Slots> frames;
    run_generator(Pattern::full, 0, 1, {f}, frames);
    CHECK(frames[2][9] == 255);
    CHECK(frames[2][10] == 128);
    CHECK(frames[2][11] == 128);
    CHECK(frames[2][12] == 255); // hue 0 = red
    CHECK(frames[2][13] == 0);
    frames.clear();
    run_generator(Pattern::off, 0, 1, {f}, frames);
    CHECK(frames.empty());
}

namespace {

struct Sender {
    asio::io_context io;
    asio::ip::udp::socket socket{io, asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), 0)};
    void send(const Bytes &b, std::uint16_t port) {
        socket.send_to(asio::buffer(b), asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    }
};

template <class F> bool eventually(F f) {
    for (int i = 0; i < 200; ++i, std::this_thread::sleep_for(5ms))
        if (f())
            return true;
    return false;
}

std::uint8_t slot(DmxEngine &e, std::uint16_t u, std::size_t i) {
    for (auto &s : e.status())
        if (s.universe == u)
            return s.data[i];
    return 0;
}

} // namespace

TEST_CASE("DmxEngine receives Art-Net and sACN on loopback and merges them") {
    DmxEngine engine({.artnet_port = 0, .sacn_port = 0, .bind_address = "127.0.0.1"});
    engine.start();
    engine.subscribe({1, 2});
    auto *tb = engine.frames(1);
    Sender tx;

    tx.send(make_artdmx(port_address_from_universe(1), 1, frame_of(512, 33)), engine.artnet_bound_port());
    REQUIRE(eventually([&] { return slot(engine, 1, 0) == 33; }));
    REQUIRE(tb->update());
    CHECK(tb->front().data[511] == 33);

    // A higher-priority sACN source on the same universe takes over.
    tx.send(make_sacn_data(cid_a, "Desk", 150, 1, 1, frame_of(512, 77)), engine.sacn_bound_port());
    REQUIRE(eventually([&] { return slot(engine, 1, 0) == 77; }));

    // Out-of-order sACN is dropped; stream-terminated removes the source and Art-Net is back.
    tx.send(make_sacn_data(cid_a, "Desk", 150, 0, 1, frame_of(512, 5)), engine.sacn_bound_port());
    tx.send(make_sacn_data(cid_a, "Desk", 150, 2, 1, frame_of(512, 5), 0, 0, true), engine.sacn_bound_port());
    REQUIRE(eventually([&] { return slot(engine, 1, 0) == 33; }));
    for (auto &s : engine.status())
        if (s.universe == 1)
            CHECK(s.dropped == 1);

    // Unsubscribed universes are ignored.
    tx.send(make_artdmx(port_address_from_universe(9), 1, frame_of(512, 1)), engine.artnet_bound_port());
    engine.set_internal_frame(2, Slots{42});
    REQUIRE(eventually([&] { return slot(engine, 2, 0) == 42; }));
    CHECK(engine.status().size() == 2);
}

TEST_CASE("DmxEngine keeps the last Art-Net timecode for a second") {
    DmxEngine engine({.sacn = false, .artnet_port = 0, .bind_address = "127.0.0.1"});
    engine.start();
    CHECK(engine.timecode().ms == -1);
    Sender tx;
    tx.send(make_arttimecode({12, 10, 0, 1, 1}), engine.artnet_bound_port());
    REQUIRE(eventually([&] { return engine.timecode().ms == 3'610'480; }));
    CHECK(engine.timecode().fps == 25);
}

TEST_CASE("DmxEngine's publish serial advances with a new frame and stays still without one (NFR-2 redraw)") {
    DmxEngine engine({.sacn = false, .artnet_port = 0, .bind_address = "127.0.0.1"});
    engine.start();
    engine.subscribe({1});
    std::this_thread::sleep_for(50ms);
    const auto idle = engine.publish_serial();
    std::this_thread::sleep_for(100ms);
    CHECK(engine.publish_serial() == idle);
    engine.set_internal_frame(1, Slots{5});
    CHECK(eventually([&] { return engine.publish_serial() > idle; }));
}

TEST_CASE("DmxEngine holds sACN frames until their sync packet") {
    DmxEngine engine({.artnet = false, .sacn_port = 0, .bind_address = "127.0.0.1"});
    engine.start();
    engine.subscribe({5});
    Sender tx;
    tx.send(make_sacn_data(cid_b, "Desk", 100, 1, 5, frame_of(512, 99), 0, 7), engine.sacn_bound_port());
    std::this_thread::sleep_for(50ms);
    CHECK(slot(engine, 5, 0) == 0);
    tx.send(make_sacn_sync(cid_b, 1, 7), engine.sacn_bound_port());
    CHECK(eventually([&] { return slot(engine, 5, 0) == 99; }));
}

TEST_CASE("DmxEngine answers ArtPoll with an ArtPollReply listing subscribed ports") {
    DmxEngine engine({.sacn = false, .artnet_port = 0, .bind_address = "127.0.0.1"});
    engine.start();
    engine.subscribe({1, 2, 3});
    Sender tx;
    std::this_thread::sleep_for(20ms);
    tx.send(make_artpoll(), engine.artnet_bound_port());
    std::array<std::uint8_t, 600> buf;
    asio::ip::udp::endpoint from;
    tx.socket.non_blocking(true);
    std::size_t n = 0;
    REQUIRE(eventually([&] {
        std::error_code ec;
        n = tx.socket.receive_from(asio::buffer(buf), from, 0, ec);
        return !ec;
    }));
    REQUIRE(n == 239);
    CHECK(buf[9] == 0x21);
    CHECK(buf[173] == 3);
    CHECK(buf[192] == 2); // universe 3 = Port-Address 2
}

namespace {
PlaybackMix recorded(Slots frame, std::uint8_t weight = 255) {
    PlaybackMix m{frame};
    m.weight.fill(weight);
    return m;
}
} // namespace

TEST_CASE("Playback mix: weight crossfades, snap switches at half, HTP takes the max, 16-bit pairs fade whole") {
    Slots live{}, rec{};
    live[0] = 0, rec[0] = 200;     // continuous
    live[1] = 3, rec[1] = 9;       // snap (a wheel)
    live[2] = 120, rec[2] = 40;    // htp
    live[3] = 0x00, live[4] = 0xFF; // 16-bit 0x00FF
    rec[3] = 0x02, rec[4] = 0x01;   // 16-bit 0x0201
    PlaybackMix m = recorded(rec, 128);
    m.snap.set(1), m.htp.set(2), m.fine.set(4);
    m.weight[5] = 0, rec[5] = 99; // live only

    std::bitset<512> from_rec;
    Slots out = mix_playback(live, m, &from_rec);
    CHECK(out[0] == 100);
    CHECK(out[1] == 9);
    CHECK(out[2] == 120);
    CHECK_FALSE(from_rec[2]);
    const int v = out[3] << 8 | out[4];
    CHECK(std::abs(v - (0x00FF + 0x0201) / 2) <= 1); // a byte-wise fade would give 0x0180
    CHECK(out[5] == 0);

    m.weight.fill(127);
    out = mix_playback(live, m, &from_rec);
    CHECK(out[1] == 3); // still live below half
    CHECK_FALSE(from_rec[1]);
    m.weight.fill(255);
    CHECK(mix_playback(live, m)[0] == 200);
}

TEST_CASE("DmxEngine playback mixes over live input, and the tap sees what is published") {
    DmxEngine engine({.artnet = false, .sacn = false});
    engine.start();
    engine.subscribe({1});
    std::atomic<int> tapped = -1;
    engine.set_tap([&](std::uint16_t u, const Slots &d) { tapped = u == 1 ? d[0] : -2; });
    engine.set_policy(1, MergePolicy::htp);
    engine.set_internal_frame(1, Slots{10}); // stands in for a live console
    REQUIRE(eventually([&] { return slot(engine, 1, 0) == 10; }));

    engine.set_playback(1, recorded(Slots{5})); // the recording beats even HTP
    CHECK(eventually([&] { return slot(engine, 1, 0) == 5; }));
    auto st = engine.status()[0];
    CHECK(st.sources == std::vector<std::string>{"Internal frame", "Playback"}); // live stays listed
    CHECK(st.winner[0] == 1);
    CHECK(tapped == 5);
    PlaybackMix both = recorded(Slots{50});
    both.htp.set();
    engine.set_playback(1, both);
    CHECK(eventually([&] { return slot(engine, 1, 0) == 50; }));
    engine.set_playback(1, recorded(Slots{50}, 0)); // weight 0: live
    CHECK(eventually([&] { return slot(engine, 1, 0) == 10; }));
    engine.set_playback(1, std::nullopt);
    CHECK(eventually([&] { return engine.status()[0].sources.size() == 1; }));
    engine.set_tap({});
}

TEST_CASE("DmxEngine output sends published universes as ArtDmx at the offset, and ignores its own packets") {
    DmxEngine engine({.sacn = false, .artnet_port = 0, .bind_address = "127.0.0.1"});
    engine.start();
    engine.subscribe({1});
    asio::io_context io;
    asio::ip::udp::socket rx{io, asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), 0)};
    rx.non_blocking(true);
    // Unicast to the test socket, then to the engine's own input: the loop guard must drop that one.
    engine.set_output({.enabled = true, .target = "127.0.0.1", .port = rx.local_endpoint().port(), .universe_offset = 4});
    engine.set_internal_frame(1, Slots{77});
    std::array<std::uint8_t, 600> buf;
    std::size_t n = 0;
    REQUIRE(eventually([&] {
        std::error_code ec;
        n = rx.receive(asio::buffer(buf), 0, ec);
        return !ec;
    }));
    auto packet = parse_artnet(std::span(buf.data(), n));
    auto *dmx = std::get_if<ArtDmx>(&packet);
    REQUIRE(dmx);
    CHECK(dmx->port_address == port_address_from_universe(5));
    CHECK(dmx->slots[0] == 77);
    CHECK(eventually([&] { return engine.output_status().packets_per_s > 0; }));

    engine.set_output({.enabled = true, .target = "127.0.0.1", .port = engine.artnet_bound_port()});
    engine.set_internal_frame(1, Slots{78});
    std::this_thread::sleep_for(100ms);
    CHECK(engine.status()[0].sources == std::vector<std::string>{"Internal frame"});
    engine.set_output({});
    CHECK(eventually([&] { return !engine.output_status().enabled; }));
}

TEST_CASE("DmxEngine sACN output carries priority and its own CID") {
    DmxEngine engine({.artnet = false, .sacn = false});
    engine.start();
    engine.subscribe({3});
    asio::io_context io;
    asio::ip::udp::socket rx{io, asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), 0)};
    rx.non_blocking(true);
    engine.set_output({.enabled = true, .sacn = true, .target = "127.0.0.1", .port = rx.local_endpoint().port(),
                       .priority = 150});
    engine.set_internal_frame(3, Slots{9});
    std::array<std::uint8_t, 700> buf;
    std::size_t n = 0;
    REQUIRE(eventually([&] {
        std::error_code ec;
        n = rx.receive(asio::buffer(buf), 0, ec);
        return !ec && n > 0;
    }));
    auto packet = parse_sacn(std::span(buf.data(), n));
    auto *d = std::get_if<SacnData>(&packet);
    REQUIRE(d);
    CHECK(d->universe == 3);
    CHECK(d->priority == 150);
    CHECK(d->slots[0] == 9);
}

TEST_CASE("DmxEngine programmer overrides only its masked slots, over live input and playback") {
    DmxEngine engine({.artnet = false, .sacn = false});
    engine.start();
    engine.subscribe({1});
    std::atomic<int> tapped = -1;
    engine.set_tap([&](std::uint16_t, const Slots &d) { tapped = d[4]; });
    Slots live;
    live.fill(10);
    engine.set_internal_frame(1, live);
    REQUIRE(eventually([&] { return slot(engine, 1, 4) == 10; }));

    Slots held{};
    held[4] = 200;
    std::bitset<512> mask;
    mask.set(4);
    engine.set_programmer(1, std::pair{held, mask});
    CHECK(eventually([&] { return slot(engine, 1, 4) == 200; }));
    CHECK(slot(engine, 1, 5) == 10);
    CHECK(tapped == 200);
    const auto st = engine.status()[0];
    CHECK(st.sources.back() == "Programmer");
    CHECK(st.winner[4] == st.sources.size() - 1);

    live.fill(20);
    engine.set_internal_frame(1, live); // unmasked slots follow live input
    CHECK(eventually([&] { return slot(engine, 1, 5) == 20; }));
    CHECK(slot(engine, 1, 4) == 200);
    engine.set_playback(1, recorded(Slots{})); // still above the recording
    CHECK(eventually([&] { return slot(engine, 1, 5) == 0; }));
    CHECK(slot(engine, 1, 4) == 200);
    engine.set_playback(1, std::nullopt);
    engine.set_programmer(1, std::nullopt);
    CHECK(eventually([&] { return slot(engine, 1, 4) == 20; }));
    CHECK(engine.status()[0].sources.size() == 1);
    engine.set_tap({});
}
