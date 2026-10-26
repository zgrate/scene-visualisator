#include "render/camera.h"
#include "render/drawing_painter.h"
#include "render/scene_builder.h"
#include "render/scene_renderer.h"
#include "render/test_pattern.h"
#include "render/transform_math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <memory>
#include <rhi/qrhi.h>

using namespace sv::render;

namespace {

FixtureLibrary bundled_library() {
    FixtureLibrary lib;
    for (const auto &e : std::filesystem::directory_iterator(SV_SOURCE_DIR "/resources/gdtf"))
        lib.emplace(e.path().filename().string(), sv::gdtf::load_gdtf(e.path()));
    return lib;
}

} // namespace

TEST_CASE("scene renderer records a frame on the Null RHI backend") {
    QRhiNullInitParams params;
    std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::Null, &params));
    REQUIRE(rhi);
    std::unique_ptr<QRhiTexture> tex(rhi->newTexture(QRhiTexture::RGBA8, {64, 64}, 1, QRhiTexture::RenderTarget));
    REQUIRE(tex->create());
    std::unique_ptr<QRhiTextureRenderTarget> rt(rhi->newTextureRenderTarget({tex.get()}));
    std::unique_ptr<QRhiRenderPassDescriptor> rp(rt->newCompatibleRenderPassDescriptor());
    rt->setRenderPassDescriptor(rp.get());
    REQUIRE(rt->create());

    const auto lib = bundled_library();
    const sv::json project = sv::demo_project();
    SceneRenderer renderer;
    renderer.set_scene(build_scene(
        project, decode_fixtures(project, lib, [](std::uint16_t) { return std::span<const std::uint8_t>(); }), {}, 0));
    QImage comp(64, 36, QImage::Format_RGBA8888); // make_composition needs fonts, so a QGuiApplication
    comp.fill(Qt::red);
    renderer.set_composition(comp);
    for (int frame = 0; frame < 2; ++frame) {
        QRhiCommandBuffer *cb = nullptr;
        REQUIRE(rhi->beginOffscreenFrame(&cb) == QRhi::FrameOpSuccess);
        renderer.initialize(rhi.get(), rt.get());
        renderer.render(cb, rt.get());
        CHECK(rhi->endOffscreenFrame() == QRhi::FrameOpSuccess);
    }
}

TEST_CASE("demo rig decodes every fixture and lights beams from a full frame") {
    const auto lib = bundled_library();
    const sv::json project = sv::demo_project();
    std::array<std::uint8_t, 512> full{};
    full.fill(255);
    const auto fixtures = decode_fixtures(project, lib, [&](std::uint16_t u) {
        return u == 1 ? std::span<const std::uint8_t>(full) : std::span<const std::uint8_t>();
    });
    CHECK(fixtures.size() == 19);
    for (const auto &[uuid, st] : fixtures)
        CHECK(st.type != nullptr);
    const RenderScene lit = build_scene(project, fixtures, {}, 0);
    CHECK(lit.beams.size() == 6 + 6 + 4 + 2 + 8);
    CHECK(lit.walls.size() == 3);

    const auto dark = decode_fixtures(project, lib, [](std::uint16_t) { return std::span<const std::uint8_t>(); });
    CHECK(build_scene(project, dark, {}, 0).beams.empty());
}

TEST_CASE("moving head beam follows pan and tilt") {
    const auto lib = bundled_library();
    sv::json project = sv::json::parse(R"({"objects":[{"uuid":"a","type":"fixture","gdtf":"Generic@Spot_7ch.gdtf",
        "mode":"Default","position":[0,6,0],"patch":{"universe":1,"address":1}}]})");
    const auto &spot = lib.at("Generic@Spot_7ch.gdtf");
    std::array<std::uint8_t, 512> frame{};
    const auto set = [&](const char *attr, std::uint8_t v) {
        for (const auto &ch : spot.modes.front().channels)
            if (ch.attribute() == attr)
                frame[ch.offsets.front()] = v; // offsets are 0-based, coarse first
    };
    for (const char *a : {"Dimmer", "ColorAdd_R", "ColorAdd_G", "ColorAdd_B"})
        set(a, 255);
    set("Pan", 128);
    set("Tilt", 128);
    const auto lookup = [&](std::uint16_t) { return std::span<const std::uint8_t>(frame); };
    RenderScene s = build_scene(project, decode_fixtures(project, lib, lookup), {}, 0);
    REQUIRE(s.beams.size() == 1);
    CHECK(s.beams[0].direction.y() == Catch::Approx(-1).margin(0.02)); // centre = straight down
    CHECK(s.spots.size() == 1);

    set("Tilt", 255); // +135 deg: past horizontal, so the beam goes up and makes no floor spot
    s = build_scene(project, decode_fixtures(project, lib, lookup), {}, 0);
    CHECK(s.beams[0].direction.y() > 0.5f);
    CHECK(s.spots.empty());
}

TEST_CASE("FR-14 beams stop on decks, shapes and the venue height; wide pools are dimmer") {
    const auto lib = bundled_library();
    sv::json project = sv::json::parse(R"({"venue":{"height":8},"objects":[{"uuid":"a","type":"fixture",
        "gdtf":"Generic@Spot_7ch.gdtf","mode":"Default","position":[0,6,0],"patch":{"universe":1,"address":1}}]})");
    sv::json deck = sv::new_deck(4, 4, 1), wall = sv::new_shape("Wall", "box", 10, 10, 0.2);
    deck["position"] = {0, 0, 0};
    wall["position"] = {0, 0, 3};
    project["objects"].push_back(deck);
    project["objects"].push_back(wall);
    const auto &spot = lib.at("Generic@Spot_7ch.gdtf");
    std::array<std::uint8_t, 512> frame{};
    const auto set = [&](const char *attr, std::uint8_t v) {
        for (const auto &ch : spot.modes.front().channels)
            if (ch.attribute() == attr)
                frame[ch.offsets.front()] = v;
    };
    for (const char *a : {"Dimmer", "ColorAdd_R", "ColorAdd_G", "ColorAdd_B"})
        set(a, 255);
    set("Pan", 128);
    set("Tilt", 128);
    const auto lookup = [&](std::uint16_t) { return std::span<const std::uint8_t>(frame); };
    const auto beam_end = [&](const RenderScene &s) {
        return s.beams[0].origin + s.beams[0].direction * s.beams[0].length;
    };
    RenderScene s = build_scene(project, decode_fixtures(project, lib, lookup), {}, 0);
    REQUIRE(s.beams.size() == 1);
    CHECK(beam_end(s).y() == Catch::Approx(1).margin(0.01)); // on the deck top, not the floor
    REQUIRE(s.spots.size() == 1);

    set("Tilt", 213); // about 90 deg toward +Z: into the wall
    s = build_scene(project, decode_fixtures(project, lib, lookup), {}, 0);
    CHECK(beam_end(s).z() == Catch::Approx(2.9).margin(0.15));
    CHECK(s.spots.empty());

    set("Tilt", 255); // up: stops at the venue height
    s = build_scene(project, decode_fixtures(project, lib, lookup), {}, 0);
    CHECK(beam_end(s).y() == Catch::Approx(8).margin(0.01));

    set("Tilt", 128); // straight down from 30 m with no deck: a wide pool, dimmed by its size
    project["objects"][0]["position"] = {10, 30, 0};
    s = build_scene(project, decode_fixtures(project, lib, lookup), {}, 0);
    REQUIRE(s.spots.size() == 1);
    CHECK(s.spots[0].colour.x() < s.beams[0].colour.x() * 0.8f);
}

TEST_CASE("FR-14 static beams follow the GDTF beam direction, scale by flux and name their fixture") {
    const auto type = [](const char *beam) {
        return sv::gdtf::parse_description(
            std::string(R"(<GDTF><FixtureType Name="X"><Geometries><Geometry Name="Base">)") + beam +
            R"(</Geometry></Geometries><DMXModes><DMXMode Name="m"><DMXChannels>
            <DMXChannel Offset="1"><LogicalChannel><ChannelFunction Attribute="Dimmer"/></LogicalChannel></DMXChannel>
            </DMXChannels></DMXMode></DMXModes></FixtureType></GDTF>)");
    };
    FixtureLibrary lib;
    lib.emplace("bright.gdtf", type(R"(<Beam Name="B" LuminousFlux="10000"/>)"));
    lib.emplace("side.gdtf", type(R"(<Geometry Name="Head" Position="{1,0,0,0}{0,0,1,0}{0,-1,0,0}{0,0,0,1}">
        <Beam Name="B" LuminousFlux="1000"/></Geometry>)"));
    const sv::json project = sv::json::parse(R"({"objects":[
        {"uuid":"t","type":"truss","position":[0,6,0]},
        {"uuid":"a","type":"fixture","gdtf":"bright.gdtf","mode":"m","position":[0,6,0],"patch":{"universe":1,"address":1}},
        {"uuid":"b","type":"fixture","gdtf":"side.gdtf","mode":"m","position":[2,6,0],"patch":{"universe":1,"address":2}}]})");
    std::array<std::uint8_t, 512> frame{};
    frame.fill(255);
    const auto s = build_scene(
        project, decode_fixtures(project, lib, [&](std::uint16_t) { return std::span<const std::uint8_t>(frame); }), {},
        0);
    REQUIRE(s.beams.size() == 2);
    const auto &bright = s.beams[0].owner == 1 ? s.beams[0] : s.beams[1];
    const auto &side = s.beams[0].owner == 2 ? s.beams[0] : s.beams[1];
    CHECK(bright.owner == 1);
    CHECK(side.owner == 2);
    CHECK(bright.direction.y() == Catch::Approx(-1).margin(1e-4));
    CHECK(side.direction.z() == Catch::Approx(1).margin(1e-4)); // horizontal toward the audience
    CHECK(side.colour.x() == Catch::Approx(bright.colour.x() * std::sqrt(0.1f)).margin(1e-3));
}

TEST_CASE("FR-14 a multi-head fixture draws one beam per head from its own yoke") {
    FixtureLibrary lib;
    lib.emplace("two.gdtf", sv::gdtf::parse_description(R"(<GDTF><FixtureType Name="X"><Geometries>
        <Geometry Name="Base">
        <Axis Name="Yoke 1" Position="{1,0,0,-0.2}{0,1,0,0}{0,0,1,0}{0,0,0,1}"><Axis Name="Head 1"><Beam Name="B1"/></Axis></Axis>
        <Axis Name="Yoke 2" Position="{1,0,0,0.2}{0,1,0,0}{0,0,1,0}{0,0,0,1}"><Axis Name="Head 2"><Beam Name="B2"/></Axis></Axis>
        </Geometry></Geometries><DMXModes><DMXMode Name="m" Geometry="Base"><DMXChannels>
        <DMXChannel Offset="1" Geometry="Base"><LogicalChannel><ChannelFunction Attribute="Dimmer"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="2" Geometry="Head 1"><LogicalChannel><ChannelFunction Attribute="Tilt" PhysicalFrom="-90" PhysicalTo="90"/></LogicalChannel></DMXChannel>
        <DMXChannel Offset="3" Geometry="Head 2"><LogicalChannel><ChannelFunction Attribute="Tilt" PhysicalFrom="-90" PhysicalTo="90"/></LogicalChannel></DMXChannel>
        </DMXChannels></DMXMode></DMXModes></FixtureType></GDTF>)"));
    const sv::json project = sv::json::parse(R"({"objects":[
        {"uuid":"a","type":"fixture","gdtf":"two.gdtf","mode":"m","position":[0,6,0],"patch":{"universe":1,"address":1}}]})");
    const std::array<std::uint8_t, 512> frame{255, 0, 255};
    const auto s = build_scene(
        project, decode_fixtures(project, lib, [&](std::uint16_t) { return std::span<const std::uint8_t>(frame); }), {},
        0);
    REQUIRE(s.beams.size() == 2);
    CHECK(s.beams[0].owner == 0);
    CHECK(s.beams[1].owner == 0);
    CHECK(s.beams[0].origin.x() == Catch::Approx(-0.2).margin(0.01));
    CHECK(s.beams[1].origin.x() == Catch::Approx(0.2).margin(0.01));
    CHECK(s.beams[0].direction.z() == Catch::Approx(-1).margin(1e-4)); // tilt -90
    CHECK(s.beams[1].direction.z() == Catch::Approx(1).margin(1e-4));  // tilt +90
}

TEST_CASE("picking returns the nearest object along the ray") {
    const sv::json project = sv::demo_project();
    const RenderScene s = build_scene(project, {}, {}, 0);
    // Straight down onto the centre wall's base area misses walls (they stand upright) but a ray at wall height hits.
    const auto hit = pick(s, {0, 2.5f, 10}, {0, 0, -1});
    REQUIRE(hit);
    const auto it = std::ranges::find_if(project["objects"], [&](const sv::json &o) { return o["uuid"] == *hit; });
    REQUIRE(it != project["objects"].end());
    CHECK((*it)["type"] == "ledWall");
    CHECK_FALSE(pick(s, {0, 50, 0}, {0, 1, 0}));
}

TEST_CASE("FR-2 deck and shapes pick by uuid; cylinder and wedge bounds fit their size") {
    sv::json project = sv::new_project();
    sv::json deck = sv::new_deck(4, 2, 1), cyl = sv::new_shape("Pillar", "cylinder", 1, 3, 1),
             wedge = sv::new_shape("Ramp", "wedge", 2, 1, 2);
    deck["position"] = {0, 0, 0};
    cyl["position"] = {5, 0, 0};
    wedge["position"] = {-5, 0, 0};
    project["objects"] = {deck, cyl, wedge};
    const RenderScene s = build_scene(project, {}, {}, 0);
    REQUIRE(s.meshes.size() == 2);
    for (const MeshInstance &m : s.meshes) {
        CHECK(m.mesh->lo.y() == Catch::Approx(-0.5f));
        CHECK(m.mesh->hi.x() == Catch::Approx(0.5f).margin(1e-4));
    }
    CHECK(s.meshes[0].model.map(QVector3D(0, 0.5f, 0)).y() == Catch::Approx(3)); // cylinder top at its height
    CHECK(pick(s, {0, 10, 0}, {0, -1, 0}) == deck["uuid"].get<std::string>());
    CHECK(pick(s, {5, 10, 0}, {0, -1, 0}) == cyl["uuid"].get<std::string>());
    CHECK(pick(s, {-5, 10, 0}, {0, -1, 0}) == wedge["uuid"].get<std::string>());
    CHECK_FALSE(pick(s, {0, 1.5f, 10}, {0, 0, -1})); // deck top at 1 m
}

TEST_CASE("world_transform applies translated and rotated parents") {
    sv::json project = sv::new_project();
    sv::json truss = sv::new_truss("T"), spot = sv::new_fixture("S", "x.gdtf", "m");
    truss["position"] = {1, 5, 0};
    truss["rotation"] = {0, 90, 0};
    spot["parent"] = truss["uuid"];
    spot["position"] = {2, 0, 0};
    project["objects"] = {truss, spot};
    const QVector3D w = world_transform(project, project["objects"][1]).map(QVector3D());
    CHECK(w.x() == Catch::Approx(1).margin(1e-4));
    CHECK(w.y() == Catch::Approx(5));
    CHECK(w.z() == Catch::Approx(-2));
}

TEST_CASE("a wall with strings draws one column per string along its polyline") {
    sv::json project = sv::new_project();
    sv::json wall = sv::new_led_wall("Net");
    wall["slice"] = {{"x", 100}, {"y", 0}, {"w", 2}, {"h", 200}};
    wall["pixelPitch"] = 10.0;
    wall["strings"] = {{{0, 3, 0}, {0, 3, -1}, {0, 3, -3}}, {{1, 3, 0}, {1, 3, -3}}};
    project["objects"] = sv::json::array({wall}); // not {x}: Apple Clang copies x instead of wrapping it
    const RenderScene s = build_scene(project, {}, {}, 0);
    REQUIRE(s.walls.size() == 3 * 4); // per segment, two crossed quads each mirrored
    const float cw = project["composition"]["width"], ch = project["composition"]["height"];
    // First string: 1 m then 2 m, so a third and two thirds of the column, starting at the slice top.
    CHECK(s.walls[0].slice.x() == Catch::Approx(100 / cw));
    CHECK(s.walls[0].slice.z() == Catch::Approx(1 / cw));
    CHECK(s.walls[0].slice.y() == Catch::Approx(0));
    CHECK(s.walls[0].slice.w() == Catch::Approx(200 / ch / 3));
    CHECK(s.walls[4].slice.y() == Catch::Approx(200 / ch / 3));
    CHECK(s.walls[8].slice.x() == Catch::Approx(101 / cw));
    // Local +Y of a segment points back to the start of its string.
    CHECK(s.walls[0].model.map(QVector3D(0, 0.5f, 0)).z() == Catch::Approx(0).margin(1e-5));
    CHECK(s.walls[0].model.map(QVector3D(0, -0.5f, 0)).z() == Catch::Approx(-1));
    CHECK(pick(s, {1, 10, -2}, {0, -1, 0}) == wall["uuid"].get<std::string>());
    // A flat string overhead is lit from below, above and the side: some quad's front faces each way.
    const auto front = [&](int k) {
        const QMatrix4x4 &m = s.walls[std::size_t(k)].model;
        return QVector3D::crossProduct(m.column(0).toVector3D(), m.column(1).toVector3D()).normalized();
    };
    for (const QVector3D look : {QVector3D(0, -1, 0), QVector3D(0, 1, 0), QVector3D(1, 0, 0), QVector3D(-1, 0, 0)}) {
        float best = 0;
        for (int k = 0; k < 4; ++k)
            best = std::max(best, QVector3D::dotProduct(front(k), look));
        CHECK(best == Catch::Approx(1).margin(1e-4));
    }
    // facing: one quad per segment turned toward that world direction.
    project["objects"][0]["facing"] = "down";
    const RenderScene d = build_scene(project, {}, {}, 0);
    REQUIRE(d.walls.size() == 3);
    for (const WallInstance &w : d.walls) {
        const QVector3D n = QVector3D::crossProduct(w.model.column(0).toVector3D(), w.model.column(1).toVector3D());
        CHECK(n.normalized().y() == Catch::Approx(-1).margin(1e-4));
    }
}

TEST_CASE("orbit camera ray goes through the target at the viewport centre") {
    OrbitCamera c;
    const auto [origin, dir] = c.ray(0.5f, 0.5f, 16.0f / 9);
    const QVector3D to_target = (c.target - origin).normalized();
    CHECK(QVector3D::dotProduct(dir, to_target) == Catch::Approx(1).margin(1e-4));
    c.zoom(1);
    CHECK(c.distance == Catch::Approx(19 * 0.9));
    c.set_preset(OrbitCamera::Preset::top);
    CHECK(c.eye().y() > c.target.y() + c.distance * 0.99f);
}

TEST_CASE("camera keys: look keeps the eye, fly moves both, back/bottom presets, flip twice is identity") {
    OrbitCamera c;
    const QVector3D e = c.eye(), t = c.target;
    c.look(20, 10);
    CHECK((c.eye() - e).length() == Catch::Approx(0).margin(1e-4));
    CHECK((c.target - t).length() > 1);
    const QVector3D forward = (c.target - c.eye()).normalized(), before = c.eye(), target = c.target;
    c.fly({0, 0, 2});
    CHECK((c.eye() - before - 2 * forward).length() == Catch::Approx(0).margin(1e-4));
    CHECK((c.target - target - 2 * forward).length() == Catch::Approx(0).margin(1e-4));
    const float yaw = c.yaw, pitch = c.pitch;
    c.flip();
    c.flip();
    CHECK(std::fmod(c.yaw - yaw + 720, 360.0f) == Catch::Approx(0).margin(1e-3));
    CHECK(c.pitch == Catch::Approx(pitch));
    c.set_preset(OrbitCamera::Preset::back);
    CHECK(c.eye().z() < c.target.z() - c.distance * 0.9f);
    c.set_preset(OrbitCamera::Preset::bottom);
    CHECK(c.eye().y() < c.target.y() - c.distance * 0.99f);
}

TEST_CASE("FR-5 axis_param, plane_hit and parse_typed") {
    // A ray straight down at x = 2 meets the X axis at t = 2.
    CHECK(*axis_param({2, 5, 1}, {0, -1, 0}, {0, 0, 0}, {1, 0, 0}) == Catch::Approx(2));
    CHECK_FALSE(axis_param({0, 1, 0}, {1, 0, 0}, {0, 0, 0}, {1, 0, 0}));
    const auto hit = plane_hit({1, 4, 1}, {0, -1, 0}, {0, 1, 0}, {0, 1, 0});
    REQUIRE(hit);
    CHECK(hit->y() == Catch::Approx(1));
    CHECK_FALSE(plane_hit({0, 4, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0})); // behind the ray
    CHECK(*parse_typed("-1.5") == -1.5);
    CHECK(*parse_typed(".25") == 0.25);
    CHECK(*parse_typed("2.") == 2);
    CHECK_FALSE(parse_typed(""));
    CHECK_FALSE(parse_typed("-"));
}

TEST_CASE("FR-5 gizmo handles exist only with one selected object and pick as gizmo:<handle>") {
    sv::json project = sv::new_project();
    sv::json a = sv::new_deck(2, 2, 1), b = sv::new_deck(2, 2, 1);
    b["position"] = {5, 0, 0};
    project["objects"] = {a, b};
    const std::string ua = a["uuid"], ub = b["uuid"];
    const auto count = [](const RenderScene &s) {
        return std::ranges::count_if(s.pick, [](const PickBox &p) { return p.uuid.starts_with("gizmo:"); });
    };
    CHECK(count(build_scene(project, {}, {ua}, 0)) == 0); // no gizmo request
    CHECK(count(build_scene(project, {}, {ua, ub, "gizmo:all"}, 0)) == 0);
    const RenderScene s = build_scene(project, {}, {ua, "gizmo:all"}, 0);
    CHECK(count(s) == 6 + 4 + 1);
    REQUIRE(s.gizmo);
    CHECK(s.gizmo->map(QVector3D()).y() == Catch::Approx(1.05f)); // on the deck's top
    // Down onto the X arrow, which lies on the deck: the handle wins over the deck.
    CHECK(pick(s, {0.6f, 10, 0}, {0, -1, 0}) == "gizmo:tx");
    CHECK(pick(s, {0.3f, 10, 0.3f}, {0, -1, 0}) == ua);
    CHECK(count(build_scene(project, {}, {ua, "gizmo:rotate"}, 0)) == 4);
}

TEST_CASE("FR-13 output feeds paint their outRect onto the wall slice; unfed walls keep the composition") {
    sv::json project = sv::new_project();
    project["outputs"] = {{{"uuid", "out-a"}, {"kind", "ndi"}, {"width", 200}, {"height", 100}},
                          {{"uuid", "out-b"}, {"kind", "ndi"}, {"width", 100}, {"height", 100}}};
    sv::json a = sv::new_led_wall("A"), b = sv::new_led_wall("B"), c = sv::new_led_wall("C");
    a["output"] = "out-a", a["slice"] = {{"x", 0}, {"y", 0}, {"w", 50}, {"h", 50}};
    a["outRect"] = {{"x", 100}, {"y", 0}, {"w", 100}, {"h", 100}};                   // the right half of output A
    b["output"] = "out-b", b["slice"] = {{"x", 50}, {"y", 0}, {"w", 50}, {"h", 50}}; // no outRect: same as slice
    c["output"] = "out-c", c["slice"] = {{"x", 0}, {"y", 50}, {"w", 50}, {"h", 50}};
    project["objects"] = {a, b, c};

    QImage base(100, 100, QImage::Format_RGBA8888);
    base.fill(Qt::black);
    QImage feed_a(400, 200, QImage::Format_RGBA8888); // twice the output size: outRect scales with the frame
    feed_a.fill(Qt::red);
    for (int y = 0; y < 200; ++y)
        for (int x = 200; x < 400; ++x)
            feed_a.setPixelColor(x, y, Qt::green);
    QImage feed_b(100, 100, QImage::Format_RGBA8888);
    feed_b.fill(Qt::blue);

    const QImage out = sv::render::compose_feeds(project, base, {{"out-a", feed_a}, {"out-b", feed_b}});
    CHECK(out.pixelColor(25, 25) == QColor(Qt::green));
    CHECK(out.pixelColor(75, 25) == QColor(Qt::blue));
    CHECK(out.pixelColor(25, 75) == QColor(Qt::black)); // out-c has no feed
    CHECK(sv::render::compose_feeds(project, base, {}).pixelColor(25, 25) == QColor(Qt::black));

    // A take's video over the live feeds: half way on out-b, out-a set to Live.
    QImage rec(100, 100, QImage::Format_RGBA8888);
    rec.fill(Qt::white);
    const QImage mixed =
        sv::render::compose_feeds(project, out, {{"out-a", rec}, {"out-b", rec}}, {{"out-a", 0.0}, {"out-b", 0.5}});
    CHECK(mixed.pixelColor(25, 25) == QColor(Qt::green));
    const QColor half = mixed.pixelColor(75, 25);
    CHECK(half.red() == Catch::Approx(128).margin(2));
    CHECK(half.blue() == 255);
}

TEST_CASE("FR-13 output feeds follow rotated slices and rotated outRects") {
    sv::json project = sv::new_project();
    project["outputs"] = {{{"uuid", "out"}, {"kind", "ndi"}, {"width", 100}, {"height", 100}}};
    sv::json a = sv::new_led_wall("A"), b = sv::new_led_wall("B");
    a["output"] = "out", a["slice"] = {{"x", 20}, {"y", 20}, {"w", 60}, {"h", 20}, {"angle", 90}};
    a["outRect"] = {{"x", 0}, {"y", 0}, {"w", 100}, {"h", 100}};
    b["output"] = "out", b["slice"] = {{"x", 0}, {"y", 60}, {"w", 100}, {"h", 40}};
    b["outRect"] = {{"x", 0}, {"y", 30}, {"w", 100}, {"h", 40}, {"angle", 90}}; // stands upright in the feed
    project["objects"] = sv::json::array({a}); // not {x}: Apple Clang copies x instead of wrapping it

    QImage base(100, 100, QImage::Format_RGBA8888);
    base.fill(Qt::black);
    QImage feed(100, 100, QImage::Format_RGBA8888); // left half red, right half green
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 100; ++x)
            feed.setPixelColor(x, y, x < 50 ? Qt::red : Qt::green);
    QImage out = sv::render::compose_feeds(project, base, {{"out", feed}});
    CHECK(out.pixelColor(50, 10) == QColor(Qt::red)); // the slice runs top to bottom, the feed's left on top
    CHECK(out.pixelColor(50, 50) == QColor(Qt::green));
    CHECK(out.pixelColor(25, 30) == QColor(Qt::black)); // outside the rotated slice

    for (int y = 0; y < 100; ++y) // top half blue, bottom half yellow
        for (int x = 0; x < 100; ++x)
            feed.setPixelColor(x, y, y < 50 ? Qt::blue : Qt::yellow);
    project["objects"] = sv::json::array({b}); // not {x}: Apple Clang copies x instead of wrapping it
    out = sv::render::compose_feeds(project, base, {{"out", feed}});
    CHECK(out.pixelColor(10, 80) == QColor(Qt::blue)); // the outRect's own x runs down the feed
    CHECK(out.pixelColor(90, 80) == QColor(Qt::yellow));
    CHECK(out.pixelColor(50, 30) == QColor(Qt::black));
}

TEST_CASE("FR-19 drawing_delta inverts drawing_point for plan, front and side") {
    const QVector3D d(1.5f, -2.0f, 0.75f);
    for (const std::string kind : {"plan", "front", "side"}) {
        const QVector3D back = drawing_delta(kind, drawing_point(kind, d));
        // the axis the view looks along is lost; the other two come back
        INFO(kind);
        CHECK(drawing_point(kind, back) == drawing_point(kind, d));
        CHECK((kind == "plan" ? back.y() : kind == "front" ? back.z() : back.x()) == 0);
    }
}

TEST_CASE("FR-19 sheet_from_json reads notes, flags and dimension offsets with defaults") {
    const DrawingSheet plain = sheet_from_json(sv::json{{"kind", "plan"}});
    CHECK(plain.notes.empty());
    CHECK(plain.auto_dimensions);
    CHECK(plain.labels);
    const DrawingSheet s =
        sheet_from_json(sv::json::parse(R"({"kind": "front", "autoDimensions": false, "labels": false,
        "notes": [{"at": [1, -2], "text": "FOH", "to": [0, 0]}],
        "dimensions": [{"a": [0, 0], "b": [1, 0], "offset": -7}]})"));
    CHECK_FALSE(s.auto_dimensions);
    CHECK_FALSE(s.labels);
    REQUIRE(s.notes.size() == 1);
    CHECK(s.notes[0]["text"] == "FOH");
    CHECK(s.dimensions[0]["offset"] == -7);
}

TEST_CASE("FR-19 paint_drawing hits a wall of strings per segment, never the stage between them") {
    sv::json project = sv::new_project();
    sv::json wall = sv::new_led_wall("Net");
    wall["strings"] = {{{0, 3, 0}, {0, 3, -1}, {0, 3, -3}}, {{1, 3, 0}, {1, 3, -3}}};
    project["objects"] = sv::json::array({wall}); // not {x}: Apple Clang copies x instead of wrapping it
    DrawingSheet sheet;
    sheet.text = false; // no fonts without a QGuiApplication
    QImage image(QSize(420, 297), QImage::Format_RGBA8888);
    QPainter p(&image);
    std::vector<DrawingHit> hits;
    const QTransform to_page = paint_drawing(p, project, FixtureLibrary{}, MeshLibrary{}, sheet, &hits);
    p.end();
    // A hit per segment, so the stage between the strings stays free to click; together they cover every string.
    REQUIRE(hits.size() == 3);
    QRectF r;
    for (const DrawingHit &h : hits) {
        CHECK(h.uuid == wall["uuid"].get<std::string>());
        r |= to_page.inverted().mapRect(h.page); // drawing metres
    }
    CHECK(r.left() < 0.1);
    CHECK(r.right() > 0.9);
    CHECK(r.top() < -2.9);
    CHECK(r.bottom() > -0.1);
}

TEST_CASE("FR-19 paint_drawing reports a hit per fixture symbol at its centre, smaller than the truss under it") {
    const sv::json project = sv::demo_project();
    const FixtureLibrary library = bundled_library();
    DrawingSheet sheet;
    sheet.text = false;
    QImage image(QSize(420, 297), QImage::Format_RGBA8888);
    QPainter p(&image);
    std::vector<DrawingHit> hits;
    const QTransform to_page = paint_drawing(p, project, library, MeshLibrary{}, sheet, &hits);
    p.end();
    std::map<std::string, QRectF> by_uuid;
    for (const DrawingHit &h : hits)
        by_uuid[h.uuid] = h.page;
    int fixtures = 0;
    for (const sv::json &o : project["objects"]) {
        if (o["type"] != "fixture")
            continue;
        ++fixtures;
        const std::string uuid = o["uuid"];
        REQUIRE(by_uuid.contains(uuid));
        const QRectF r = by_uuid[uuid];
        CHECK(r.width() == Catch::Approx(4.4));
        const QPointF c = to_page.inverted().map(r.center()); // drawing metres
        const QVector3D world = world_transform(project, o).map(QVector3D());
        CHECK(c.x() == Catch::Approx(world.x()).margin(0.3));
        CHECK(c.y() == Catch::Approx(world.z()).margin(0.3));
        if (o.contains("parent") && o["parent"].is_string() && by_uuid.contains(o["parent"].get<std::string>())) {
            const QRectF parent = by_uuid[o["parent"].get<std::string>()];
            CHECK(parent.width() * parent.height() > r.width() * r.height());
        }
    }
    CHECK(fixtures > 0);
}

TEST_CASE("A running hazer raises the scene haze above the venue minimum, never below it") {
    sv::json project = sv::new_project();
    project["venue"]["haze"] = 0.2;
    std::map<std::string, FixtureState> fixtures;
    CHECK(build_scene(project, fixtures, {}, 0).haze == Catch::Approx(0.2));
    fixtures["hazer"].attrs.haze = 1;
    CHECK(build_scene(project, fixtures, {}, 0).haze == Catch::Approx(0.8));
    fixtures["hazer"].attrs.haze = 0.1;
    CHECK(build_scene(project, fixtures, {}, 0).haze == Catch::Approx(0.2));
}

TEST_CASE("sample_slice takes LED centres from a scaled, rotated slice, row- or column-major") {
    const sv::json project = {{"composition", {{"width", 100}, {"height", 100}}}};
    QImage img(200, 200, QImage::Format_RGB32); // twice the composition, as when it is capped
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            img.setPixel(x, y, qRgb(x, y, 0));
    const sv::json slice = {{"x", 30}, {"y", 40}, {"w", 40}, {"h", 20}};
    auto px = sample_slice(img, project, slice, 4, 2);
    REQUIRE(px.size() == 4 * 2 * 3);
    CHECK((px[0] == 70 && px[1] == 90)); // LED (0,0) at composition (35, 45)
    CHECK((px[3] == 90 && px[4] == 90)); // LED (1,0) at (45, 45)
    CHECK((px[12] == 70 && px[13] == 110)); // second row starts at (35, 55)
    px = sample_slice(img, project, slice, 4, 2, true);
    CHECK((px[3] == 70 && px[4] == 110)); // column-major: LED (0,1) comes second
    auto turned = slice;
    turned["angle"] = 90;
    px = sample_slice(img, project, turned, 4, 2);
    CHECK((px[0] == 110 && px[1] == 70)); // (-15, -5) about (50, 50) turned 90° lands on (55, 35)
    CHECK(sample_slice(QImage(), project, slice, 4, 2).empty());
}
