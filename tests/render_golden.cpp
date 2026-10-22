// Headless render smoke test (M4): the demo rig with a generator frame, rendered through QRhi into a PNG and
// compared against tests/golden/demo.png. Usage: sv_render_golden <golden.png> [--no-compare]
// SV_UPDATE_GOLDEN=1 (or a missing golden) rewrites the golden instead.
#include "render/scene_builder.h"
#include "render/scene_renderer.h"
#include "render/test_pattern.h"

#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <rhi/qrhi.h>

using namespace sv::render;

namespace {

QImage render_demo(QRhi *rhi, QSize size) {
    std::unique_ptr<QRhiTexture> colour(
        rhi->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    colour->create();
    std::unique_ptr<QRhiRenderBuffer> depth(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size));
    depth->create();
    QRhiTextureRenderTargetDescription desc{QRhiColorAttachment(colour.get())};
    desc.setDepthStencilBuffer(depth.get());
    std::unique_ptr<QRhiTextureRenderTarget> rt(rhi->newTextureRenderTarget(desc));
    std::unique_ptr<QRhiRenderPassDescriptor> rp(rt->newCompatibleRenderPassDescriptor());
    rt->setRenderPassDescriptor(rp.get());
    rt->create();

    FixtureLibrary lib;
    for (const auto &e : std::filesystem::directory_iterator(SV_SOURCE_DIR "/resources/gdtf"))
        lib.emplace(e.path().filename().string(), sv::gdtf::load_gdtf(e.path()));
    const sv::json project = sv::demo_project();
    std::map<std::uint16_t, sv::proto::Slots> frames;
    sv::proto::run_generator(sv::proto::Pattern::rainbow, 1.5, 1, generator_targets(project, lib), frames);
    const auto fixtures = decode_fixtures(project, lib, [&](std::uint16_t u) {
        auto it = frames.find(u);
        return it == frames.end() ? std::span<const std::uint8_t>() : std::span<const std::uint8_t>(it->second);
    });

    SceneRenderer renderer;
    renderer.set_scene(build_scene(project, fixtures, {}, 0));
    renderer.set_composition(make_composition(project));
    renderer.set_camera(OrbitCamera{}, 0);

    QRhiReadbackResult result;
    QRhiCommandBuffer *cb = nullptr;
    if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
        return {};
    renderer.initialize(rhi, rt.get());
    renderer.render(cb, rt.get());
    QRhiResourceUpdateBatch *u = rhi->nextResourceUpdateBatch();
    u->readBackTexture({colour.get()}, &result);
    cb->resourceUpdate(u);
    rhi->endOffscreenFrame();
    QImage img(reinterpret_cast<const uchar *>(result.data.constData()), result.pixelSize.width(),
               result.pixelSize.height(), QImage::Format_RGBA8888);
    img = img.copy();
    return rhi->isYUpInFramebuffer() ? img.flipped() : img;
}

} // namespace

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: sv_render_golden <golden.png> [--no-compare]\n");
        return 2;
    }
    const QString golden = QString::fromLocal8Bit(argv[1]);
    const bool compare = !(argc > 2 && QByteArray(argv[2]) == "--no-compare");

    // shortcut: OpenGL only (llvmpipe here, CI GL drivers elsewhere); add D3D12/WARP and Metal runs with the CI matrix.
    QRhiGles2InitParams params;
    params.fallbackSurface = QRhiGles2InitParams::newFallbackSurface();
    std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::OpenGLES2, &params));
    if (!rhi) {
        std::fprintf(stderr, "no OpenGL QRhi, skipping\n");
        return 77; // ctest SKIP_RETURN_CODE
    }
    const QImage actual = render_demo(rhi.get(), {640, 360});
    if (actual.isNull())
        return 2;
    actual.save("render_golden_actual.png");

    if (!compare)
        return 0;
    if (qEnvironmentVariableIntValue("SV_UPDATE_GOLDEN") || !QFile::exists(golden)) {
        std::printf("writing golden %s\n", qPrintable(golden));
        return actual.save(golden) ? 0 : 1;
    }
    const QImage expected = QImage(golden).convertToFormat(QImage::Format_RGBA8888);
    if (expected.size() != actual.size()) {
        std::fprintf(stderr, "size mismatch\n");
        return 1;
    }
    double sum = 0;
    long bad = 0;
    for (int y = 0; y < actual.height(); ++y) {
        const uchar *a = actual.constScanLine(y), *e = expected.constScanLine(y);
        for (int x = 0; x < actual.width() * 4; x += 4) {
            int worst = 0;
            for (int c = 0; c < 3; ++c) {
                const int d = std::abs(a[x + c] - e[x + c]);
                sum += d, worst = std::max(worst, d);
            }
            bad += worst > 48;
        }
    }
    const double pixels = double(actual.width()) * actual.height();
    const double mean = sum / (pixels * 3), bad_share = double(bad) / pixels;
    std::printf("mean abs diff %.3f, pixels off by >48: %.3f%%\n", mean, bad_share * 100);
    return mean < 2.0 && bad_share < 0.005 ? 0 : 1;
}
