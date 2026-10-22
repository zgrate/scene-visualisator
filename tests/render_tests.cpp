#include "render/scene_renderer.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <rhi/qrhi.h>

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

    QRhiCommandBuffer *cb = nullptr;
    REQUIRE(rhi->beginOffscreenFrame(&cb) == QRhi::FrameOpSuccess);
    sv::render::SceneRenderer{}.render(cb, rt.get());
    CHECK(rhi->endOffscreenFrame() == QRhi::FrameOpSuccess);
}
