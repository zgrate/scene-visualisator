#include "render/camera.h"
#include "render/scene_builder.h"
#include "render/scene_renderer.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
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
