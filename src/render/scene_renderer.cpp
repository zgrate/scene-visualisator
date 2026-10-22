#include "render/scene_renderer.h"

#include <QFile>
#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <rhi/qrhi.h>

namespace sv::render {

namespace {

struct FrameUniforms { // matches the std140 Frame block in shaders/*
    float view_proj[16];
    float eye[4], light[4], floor_info[4], venue[4];
    float spot_pos[32][4], spot_col[32][4];
};
static_assert(sizeof(FrameUniforms) == 1152);

struct Vertex {
    float p[3], n[3];
};

std::vector<Vertex> cube_vertices() {
    std::vector<Vertex> out;
    // Per face: normal n and in-plane axes u, v with u x v = n, so triangles are CCW from outside.
    const QVector3D faces[6][3] = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
                                   {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
                                   {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}}, {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}}};
    for (const auto &[n, u, v] : faces) {
        const auto corner = [&](float a, float b) {
            const QVector3D p = 0.5f * (n + a * u + b * v);
            return Vertex{{p.x(), p.y(), p.z()}, {n.x(), n.y(), n.z()}};
        };
        for (auto [a, b] : {std::pair{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}})
            out.push_back(corner(float(a), float(b)));
    }
    return out;
}

std::vector<Vertex> quad_vertices() { // unit quad in XY facing +Z
    std::vector<Vertex> out;
    for (auto [x, y] : {std::pair{-.5f, -.5f}, {.5f, -.5f}, {.5f, .5f}, {-.5f, -.5f}, {.5f, .5f}, {-.5f, .5f}})
        out.push_back({{x, y, 0}, {0, 0, 1}});
    return out;
}

std::vector<Vertex> cone_vertices(int segments = 24) { // apex at origin, base radius 1 at z = 1, CCW outward
    std::vector<Vertex> out;
    const auto ring = [&](int i) {
        const float a = 2 * std::numbers::pi_v<float> * float(i) / float(segments);
        return Vertex{{std::cos(a), std::sin(a), 1}, {}};
    };
    for (int i = 0; i < segments; ++i) {
        out.insert(out.end(), {Vertex{}, ring(i + 1), ring(i)});
        out.insert(out.end(), {Vertex{{0, 0, 1}, {}}, ring(i), ring(i + 1)});
    }
    return out;
}

QShader load_shader(const char *name) {
    QFile f(QStringLiteral(":/sv/shaders/%1.qsb").arg(QLatin1StringView(name)));
    if (!f.open(QIODevice::ReadOnly))
        qFatal("missing shader %s", name); // build error, not a runtime condition
    return QShader::fromSerialized(f.readAll());
}

void put(float *dst, const QMatrix4x4 &m) { std::copy_n(m.constData(), 16, dst); }

} // namespace

struct SceneRenderer::Gpu {
    QRhi *rhi = nullptr;
    QVector<quint32> format;
    int samples = 0;
    std::unique_ptr<QRhiBuffer> ubuf, cube, quad, cone, boxes, walls, beams;
    std::unique_ptr<QRhiTexture> texture;
    std::unique_ptr<QRhiSampler> sampler;
    std::unique_ptr<QRhiShaderResourceBindings> srb;
    std::unique_ptr<QRhiGraphicsPipeline> grid, mesh, piece, wall, beam;
    int cone_count = 0;
    std::unique_ptr<QRhiBuffer> piece_instances;
    // Set-piece vertex buffers, uploaded once per mesh and dropped when no longer in the scene.
    std::map<std::shared_ptr<const Mesh>, std::unique_ptr<QRhiBuffer>> pieces;

    std::unique_ptr<QRhiBuffer> immutable(QRhiResourceUpdateBatch *u, const std::vector<Vertex> &v) {
        std::unique_ptr<QRhiBuffer> b(
            rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, quint32(v.size() * sizeof(Vertex))));
        b->create();
        u->uploadStaticBuffer(b.get(), v.data());
        return b;
    }

    // Grows a dynamic instance buffer as needed. QRhi defers releasing the old one until the GPU is done with it.
    void stream(QRhiResourceUpdateBatch *u, std::unique_ptr<QRhiBuffer> &b, const std::vector<float> &data) {
        const quint32 size = quint32(std::max<std::size_t>(data.size() * sizeof(float), 256));
        if (!b || b->size() < size) {
            b.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, size + size / 2));
            b->create();
        }
        if (!data.empty())
            u->updateDynamicBuffer(b.get(), 0, quint32(data.size() * sizeof(float)), data.data());
    }

    std::unique_ptr<QRhiGraphicsPipeline> pipeline(QRhiRenderPassDescriptor *rp, const char *shader,
                                                   std::initializer_list<QRhiVertexInputBinding> bindings,
                                                   std::initializer_list<QRhiVertexInputAttribute> attributes,
                                                   QRhiGraphicsPipeline::CullMode cull, bool additive) {
        std::unique_ptr<QRhiGraphicsPipeline> p(rhi->newGraphicsPipeline());
        const QByteArray name(shader);
        p->setShaderStages({{QRhiShaderStage::Vertex, load_shader(name + ".vert")},
                            {QRhiShaderStage::Fragment, load_shader(name + ".frag")}});
        QRhiVertexInputLayout layout;
        layout.setBindings(bindings);
        layout.setAttributes(attributes);
        p->setVertexInputLayout(layout);
        p->setShaderResourceBindings(srb.get());
        p->setRenderPassDescriptor(rp);
        p->setSampleCount(samples);
        p->setDepthTest(true);
        p->setDepthWrite(!additive);
        p->setCullMode(cull);
        if (additive) {
            QRhiGraphicsPipeline::TargetBlend blend;
            blend.enable = true;
            blend.srcColor = blend.dstColor = QRhiGraphicsPipeline::One;
            blend.srcAlpha = QRhiGraphicsPipeline::Zero;
            blend.dstAlpha = QRhiGraphicsPipeline::One;
            p->setTargetBlends({blend});
        }
        if (!p->create())
            qFatal("pipeline %s failed", shader);
        return p;
    }

    void build_pipelines(QRhiRenderPassDescriptor *rp) {
        using A = QRhiVertexInputAttribute;
        using B = QRhiVertexInputBinding;
        constexpr quint32 vs = sizeof(Vertex);
        const B mesh_vertex(vs);
        const A pos(0, 0, A::Float3, 0), normal(0, 1, A::Float3, 12);
        const auto mat = [](int binding, int loc) {
            return std::array{A(binding, loc, A::Float4, 0), A(binding, loc + 1, A::Float4, 16),
                              A(binding, loc + 2, A::Float4, 32), A(binding, loc + 3, A::Float4, 48)};
        };
        const auto m = mat(1, 2);
        grid = pipeline(rp, "grid", {mesh_vertex}, {pos, normal}, QRhiGraphicsPipeline::None, false);
        mesh =
            pipeline(rp, "mesh", {mesh_vertex, B(80, B::PerInstance)},
                     {pos, normal, m[0], m[1], m[2], m[3], A(1, 6, A::Float4, 64)}, QRhiGraphicsPipeline::Back, false);
        // Imported OBJ winding is not reliable, so set pieces are drawn double-sided.
        piece =
            pipeline(rp, "mesh", {mesh_vertex, B(80, B::PerInstance)},
                     {pos, normal, m[0], m[1], m[2], m[3], A(1, 6, A::Float4, 64)}, QRhiGraphicsPipeline::None, false);
        wall = pipeline(rp, "wall", {mesh_vertex, B(112, B::PerInstance)},
                        {pos, normal, m[0], m[1], m[2], m[3], A(1, 6, A::Float4, 64), A(1, 7, A::Float4, 80),
                         A(1, 8, A::Float4, 96)},
                        QRhiGraphicsPipeline::None, false);
        beam = pipeline(rp, "beam", {mesh_vertex, B(48, B::PerInstance)},
                        {pos, A(1, 1, A::Float4, 0), A(1, 2, A::Float4, 16), A(1, 3, A::Float4, 32)},
                        QRhiGraphicsPipeline::Front, true);
    }
};

SceneRenderer::SceneRenderer() = default;
SceneRenderer::~SceneRenderer() = default;

void SceneRenderer::set_scene(RenderScene s) {
    scene = std::move(s);
    scene_dirty = true;
}

void SceneRenderer::set_composition(QImage image) {
    composition = std::move(image).convertToFormat(QImage::Format_RGBA8888);
    composition_dirty = true;
}

void SceneRenderer::set_camera(const OrbitCamera &c, double time_s) {
    camera = c;
    time = time_s;
}

void SceneRenderer::initialize(QRhi *rhi, QRhiRenderTarget *rt) {
    if (!gpu || gpu->rhi != rhi) {
        gpu = std::make_unique<Gpu>();
        gpu->rhi = rhi;
        scene_dirty = composition_dirty = true;
    }
    Gpu &g = *gpu;
    const QVector<quint32> format = rt->renderPassDescriptor()->serializedFormat();
    if (g.grid && format == g.format && rt->sampleCount() == g.samples)
        return;
    g.format = format;
    g.samples = rt->sampleCount();
    if (!g.ubuf) {
        g.ubuf.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                    quint32(rhi->ubufAligned(sizeof(FrameUniforms)))));
        g.ubuf->create();
        g.sampler.reset(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                        QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        g.sampler->create();
        g.texture.reset(rhi->newTexture(QRhiTexture::RGBA8, {16, 16}));
        g.texture->create();
        g.srb.reset(rhi->newShaderResourceBindings());
        g.srb->setBindings(
            {QRhiShaderResourceBinding::uniformBuffer(
                 0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, g.ubuf.get()),
             QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, g.texture.get(),
                                                       g.sampler.get())});
        g.srb->create();
    }
    g.build_pipelines(rt->renderPassDescriptor());
}

void SceneRenderer::render(QRhiCommandBuffer *cb, QRhiRenderTarget *rt) {
    Gpu &g = *gpu;
    QRhi *rhi = g.rhi;
    QRhiResourceUpdateBatch *u = rhi->nextResourceUpdateBatch();
    if (!g.cube) {
        g.cube = g.immutable(u, cube_vertices());
        g.quad = g.immutable(u, quad_vertices());
        const auto cone = cone_vertices();
        g.cone = g.immutable(u, cone);
        g.cone_count = int(cone.size());
    }

    if (composition_dirty && !composition.isNull()) {
        if (g.texture->pixelSize() != composition.size()) {
            g.texture->setPixelSize(composition.size());
            g.texture->create(); // same object, so the SRB stays valid
        }
        u->uploadTexture(g.texture.get(), composition);
        composition_dirty = false;
    }

    if (scene_dirty) {
        std::vector<float> boxes, walls, beams;
        boxes.reserve(scene.boxes.size() * 20);
        for (const auto &b : scene.boxes) {
            boxes.resize(boxes.size() + 20);
            float *p = boxes.data() + boxes.size() - 20;
            put(p, b.model);
            std::copy_n(std::array{b.colour.x(), b.colour.y(), b.colour.z(), b.colour.w()}.data(), 4, p + 16);
        }
        for (const auto &w : scene.walls) {
            walls.resize(walls.size() + 28);
            float *p = walls.data() + walls.size() - 28;
            put(p, w.model);
            for (int i = 0; i < 4; ++i)
                p[16 + i] = w.slice[i], p[20 + i] = w.grid[i];
            p[24] = w.brightness, p[25] = w.selected ? 1.0f : 0.0f, p[26] = w.angle, p[27] = w.aspect;
        }
        for (const auto &b : scene.beams)
            beams.insert(beams.end(),
                         {b.origin.x(), b.origin.y(), b.origin.z(), b.length, b.direction.x(), b.direction.y(),
                          b.direction.z(), b.tan_half_angle, b.colour.x(), b.colour.y(), b.colour.z(), 1});
        g.stream(u, g.boxes, boxes);
        g.stream(u, g.walls, walls);
        g.stream(u, g.beams, beams);
        std::vector<float> pieces;
        std::erase_if(g.pieces, [&](const auto &e) {
            return std::ranges::none_of(scene.meshes, [&](const MeshInstance &m) { return m.mesh == e.first; });
        });
        for (const auto &m : scene.meshes) {
            pieces.resize(pieces.size() + 20);
            float *p = pieces.data() + pieces.size() - 20;
            put(p, m.model);
            std::copy_n(std::array{m.colour.x(), m.colour.y(), m.colour.z(), m.colour.w()}.data(), 4, p + 16);
            auto &buf = g.pieces[m.mesh];
            if (!buf && !m.mesh->vertices.empty()) {
                buf.reset(rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                                         quint32(m.mesh->vertices.size() * sizeof(float))));
                buf->create();
                u->uploadStaticBuffer(buf.get(), m.mesh->vertices.data());
            }
        }
        g.stream(u, g.piece_instances, pieces);
        scene_dirty = false;
    }

    const QSize size = rt->pixelSize();
    FrameUniforms f{};
    put(f.view_proj, rhi->clipSpaceCorrMatrix() * camera.projection(float(size.width()) / std::max(size.height(), 1)) *
                         camera.view());
    const QVector3D eye = camera.eye(), light = QVector3D(0.4f, 1.0f, 0.6f).normalized();
    std::copy_n(std::array{eye.x(), eye.y(), eye.z(), float(time)}.data(), 4, f.eye);
    std::copy_n(std::array{light.x(), light.y(), light.z(), 0.f}.data(), 4, f.light);
    const int spots = int(std::min<std::size_t>(scene.spots.size(), 32));
    std::copy_n(std::array{scene.venue_width / 2, scene.venue_depth / 2, scene.haze, float(spots)}.data(), 4,
                f.floor_info);
    for (int i = 0; i < spots; ++i) {
        const auto &s = scene.spots[std::size_t(i)];
        std::copy_n(std::array{s.centre.x(), s.centre.y(), s.radius, 0.f}.data(), 4, f.spot_pos[i]);
        std::copy_n(std::array{s.colour.x(), s.colour.y(), s.colour.z(), 0.f}.data(), 4, f.spot_col[i]);
    }
    u->updateDynamicBuffer(g.ubuf.get(), 0, sizeof f, &f);

    cb->beginPass(rt, clear_color, {1.0f, 0}, u);
    cb->setViewport({0, 0, float(size.width()), float(size.height())});
    const auto draw = [&](QRhiGraphicsPipeline *p, QRhiBuffer *mesh, quint32 count, QRhiBuffer *inst,
                          std::size_t instances) {
        if (instances == 0)
            return;
        cb->setGraphicsPipeline(p);
        cb->setShaderResources();
        if (inst) {
            const QRhiCommandBuffer::VertexInput in[] = {{mesh, 0}, {inst, 0}};
            cb->setVertexInput(0, 2, in);
        } else {
            const QRhiCommandBuffer::VertexInput in[] = {{mesh, 0}};
            cb->setVertexInput(0, 1, in);
        }
        cb->draw(count, quint32(instances));
    };
    draw(g.grid.get(), g.quad.get(), 6, nullptr, 1);
    draw(g.mesh.get(), g.cube.get(), 36, g.boxes.get(), scene.boxes.size());
    for (std::size_t i = 0; i < scene.meshes.size(); ++i) // one draw per piece; the instance offset picks its matrix
        if (const auto &buf = g.pieces[scene.meshes[i].mesh]) {
            cb->setGraphicsPipeline(g.piece.get());
            cb->setShaderResources();
            const QRhiCommandBuffer::VertexInput in[] = {{buf.get(), 0}, {g.piece_instances.get(), quint32(i * 80)}};
            cb->setVertexInput(0, 2, in);
            cb->draw(quint32(scene.meshes[i].mesh->vertices.size() / 6));
        }
    draw(g.wall.get(), g.quad.get(), 6, g.walls.get(), scene.walls.size());
    draw(g.beam.get(), g.cone.get(), quint32(g.cone_count), g.beams.get(), scene.beams.size());
    cb->endPass();
}

} // namespace sv::render
