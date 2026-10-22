#include "viewport_item.h"

#include "render/scene_renderer.h"

namespace {

class ViewportRenderer : public QQuickRhiItemRenderer {
    void initialize(QRhiCommandBuffer *) override {}
    void synchronize(QQuickRhiItem *) override {}
    void render(QRhiCommandBuffer *cb) override { scene.render(cb, renderTarget()); }

    sv::render::SceneRenderer scene;
};

} // namespace

QQuickRhiItemRenderer *ViewportItem::createRenderer() { return new ViewportRenderer; }
