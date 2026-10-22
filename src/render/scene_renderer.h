#pragma once
#include <QColor>

class QRhiCommandBuffer;
class QRhiRenderTarget;

namespace sv::render {

// Draws one frame of the scene into a render target. Owned and driven by the render thread.
class SceneRenderer {
  public:
    void render(QRhiCommandBuffer *cb, QRhiRenderTarget *rt);

    QColor clear_color{0x0b, 0x0c, 0x10}; // Theme viewport token
};

} // namespace sv::render
