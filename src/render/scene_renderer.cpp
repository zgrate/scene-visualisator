#include "render/scene_renderer.h"

#include <rhi/qrhi.h>

namespace sv::render {

void SceneRenderer::render(QRhiCommandBuffer *cb, QRhiRenderTarget *rt) {
    cb->beginPass(rt, clear_color, {1.0f, 0});
    cb->endPass();
}

} // namespace sv::render
