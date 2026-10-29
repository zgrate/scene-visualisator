#pragma once
// QRhi renderer for a RenderScene: floor grid, instanced boxes, LED walls fed by the composition texture,
// and additive volumetric beam cones. Owned and driven by the render thread. With `hdr` the scene renders into a
// 16-bit float target, then bloom and a highlight roll-off composite it into the caller's target. `cinematic` adds
// rig lighting for the offline render.
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
    bool hdr = true;                      // preference renderQuality: high (true) or fast
    // Offline render only (implies hdr): beams and LED walls light the rig, the floor reflects the walls, beams are
    // ray-marched, and a filmic tone map replaces the Theme-exact roll-off. Set before the first initialize().
    bool cinematic = false;
    bool hdr_active() const; // hdr, and the backend can render RGBA16F

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
