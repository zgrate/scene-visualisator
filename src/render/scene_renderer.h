#pragma once
// QRhi renderer for a RenderScene: floor grid, instanced boxes, LED walls fed by the composition texture,
// and additive volumetric beam cones. Owned and driven by the render thread.
#include "render/camera.h"
#include "render/scene_builder.h"

#include <QColor>
#include <QImage>
#include <memory>

class QRhi;
class QRhiCommandBuffer;
class QRhiRenderTarget;

namespace sv::render {

class SceneRenderer {
  public:
    SceneRenderer();
    ~SceneRenderer();

    // Call before render() each frame; rebuilds pipelines only when the target's format or sample count changed.
    void initialize(QRhi *rhi, QRhiRenderTarget *rt);
    void set_scene(RenderScene scene);
    void set_composition(QImage image);
    void set_camera(const OrbitCamera &camera, double time_s);
    void render(QRhiCommandBuffer *cb, QRhiRenderTarget *rt);

    QColor clear_color{0x0b, 0x0c, 0x10}; // Theme viewport token

  private:
    struct Gpu;
    std::unique_ptr<Gpu> gpu;
    RenderScene scene;
    QImage composition;
    bool scene_dirty = true, composition_dirty = true;
    OrbitCamera camera;
    double time = 0;
};

} // namespace sv::render
