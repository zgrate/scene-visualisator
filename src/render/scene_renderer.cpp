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

constexpr int max_beam_lights = 48, max_wall_cells = 64, max_wall_quads = 16;
struct LightUniforms { // matches the std140 Lights block in shaders/lights.glsl (cinematic only)
    float counts[4];   // beams, wall cells, wall quads
    float beam_pos[max_beam_lights][4], beam_dir[max_beam_lights][4], beam_col[max_beam_lights][4];
    float cell_pos[max_wall_cells][4], cell_dir[max_wall_cells][4], cell_col[max_wall_cells][4];
    float wall_c[max_wall_quads][4], wall_x[max_wall_quads][4], wall_y[max_wall_quads][4];
    float wall_slice[max_wall_quads][4], wall_params[max_wall_quads][4];
};

struct PostUniforms { // matches the std140 Post block in shaders/post.vert and the post fragment shaders
    float texel[4], params[4];
};
constexpr int bloom_mips = 6;
constexpr float bloom_strength = 0.22f, bloom_threshold = 0.85f, cinematic_bloom = 0.32f;

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
        const auto corner = [n = n, u = u, v = v](float a, float b) { // Apple Clang 15: no binding captures
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
void put4(float *dst, QVector3D v, float w) { std::copy_n(std::array{v.x(), v.y(), v.z(), w}.data(), 4, dst); }

// The rig as lights for the cinematic shaders: the brightest beams as spot lights, the largest walls split into cells
// of their mean colour (from `small`, a downscaled composition) as area lights, and the largest wall quads for the
// floor's reflections.
LightUniforms rig_lights(const RenderScene &scene, const QImage &small, int composition_width) {
    LightUniforms l{};
    std::vector<const BeamInstance *> beams;
    for (const auto &b : scene.beams)
        if (std::max({b.colour.x(), b.colour.y(), b.colour.z()}) > 0.004f)
            beams.push_back(&b);
    const auto peak = [](const BeamInstance *b) { return std::max({b->colour.x(), b->colour.y(), b->colour.z()}); };
    std::ranges::sort(beams, [&](auto *a, auto *b) { return peak(a) > peak(b); });
    const int nb = int(std::min<std::size_t>(beams.size(), max_beam_lights));
    for (int i = 0; i < nb; ++i) {
        put4(l.beam_pos[i], beams[i]->origin, beams[i]->length);
        put4(l.beam_dir[i], beams[i]->direction.normalized(), beams[i]->tan_half_angle);
        put4(l.beam_col[i], beams[i]->colour, 0);
    }
    std::vector<const WallInstance *> walls;
    for (const auto &w : scene.walls)
        walls.push_back(&w);
    const auto area = [](const WallInstance *w) {
        return w->model.column(0).toVector3D().length() * w->model.column(1).toVector3D().length();
    };
    std::ranges::sort(walls, [&](auto *a, auto *b) { return area(a) > area(b); });
    walls.resize(std::min<std::size_t>(walls.size(), max_wall_cells));
    const int per = walls.empty() ? 0 : max_wall_cells / int(walls.size());
    const int cols = per >= 4 ? (per >= 8 ? 4 : 2) : (per >= 2 ? 2 : 1), rows = per >= 4 ? 2 : 1;
    // Mean colour of a sub-rect of the composition (0..1); the slice angle is ignored.
    const auto mean = [&](float x, float y, float w, float h) {
        if (small.isNull())
            return QVector3D();
        const int x0 = std::clamp(int(x * small.width()), 0, small.width() - 1),
                  y0 = std::clamp(int(y * small.height()), 0, small.height() - 1);
        const int x1 = std::clamp(int(std::ceil((x + w) * small.width())), x0 + 1, small.width()),
                  y1 = std::clamp(int(std::ceil((y + h) * small.height())), y0 + 1, small.height());
        QVector3D sum;
        for (int yy = y0; yy < y1; ++yy) {
            const uchar *row = small.constScanLine(yy);
            for (int xx = x0; xx < x1; ++xx)
                sum += QVector3D(row[xx * 4], row[xx * 4 + 1], row[xx * 4 + 2]);
        }
        return sum / (255.0f * float((x1 - x0) * (y1 - y0)));
    };
    int nc = 0, nw = 0;
    for (const WallInstance *w : walls) {
        const QVector3D c = w->model.column(3).toVector3D(), ax = w->model.column(0).toVector3D(),
                        ay = w->model.column(1).toVector3D(), n = QVector3D::crossProduct(ax, ay).normalized();
        const float cell_area = area(w) / float(cols * rows);
        for (int r = 0; r < rows && nc < max_wall_cells; ++r)
            for (int k = 0; k < cols && nc < max_wall_cells; ++k, ++nc) {
                const float fx = (float(k) + 0.5f) / float(cols), fy = (float(r) + 0.5f) / float(rows);
                put4(l.cell_pos[nc], c + ax * (fx - 0.5f) + ay * (0.5f - fy), cell_area);
                put4(l.cell_dir[nc], n, 0);
                const QVector4D &s = w->slice;
                put4(l.cell_col[nc],
                     mean(s.x() + s.z() * float(k) / float(cols), s.y() + s.w() * float(r) / float(rows),
                          s.z() / float(cols), s.w() / float(rows)) *
                         w->brightness,
                     0);
            }
        if (nw < max_wall_quads) {
            put4(l.wall_c[nw], c, 0);
            put4(l.wall_x[nw], ax, 0);
            put4(l.wall_y[nw], ay, 0);
            std::copy_n(std::array{w->slice.x(), w->slice.y(), w->slice.z(), w->slice.w()}.data(), 4, l.wall_slice[nw]);
            const float texels_per_m = float(composition_width) * w->slice.z() / std::max(ax.length(), 1e-3f);
            std::copy_n(std::array{w->angle, w->aspect, w->brightness, texels_per_m}.data(), 4, l.wall_params[nw]);
            ++nw;
        }
    }
    std::copy_n(std::array{float(nb), float(nc), float(nw), 0.f}.data(), 4, l.counts);
    return l;
}

} // namespace

struct SceneRenderer::Gpu {
    QRhi *rhi = nullptr;
    QVector<quint32> format;
    int samples = 0, target_samples = 0; // the scene passes' sample count; the caller's target's
    bool hdr = false, cinematic = false;
    QImage small; // the composition downscaled, for the wall cells' colours (cinematic)
    // HDR path: the scene renders into `msaa` (resolved to `scene`), bloom runs down and up `mips`, and the
    // composite pass writes the caller's target. Sizes follow the caller's target; see resize().
    QSize size;
    std::unique_ptr<QRhiRenderBuffer> msaa, depth;
    std::unique_ptr<QRhiTexture> scene;
    std::unique_ptr<QRhiTextureRenderTarget> scene_rt;
    std::unique_ptr<QRhiRenderPassDescriptor> scene_rp, mip_rp, mip_add_rp;
    std::array<std::unique_ptr<QRhiTexture>, bloom_mips> mips;
    std::array<std::unique_ptr<QRhiTextureRenderTarget>, bloom_mips> mip_rt, mip_add_rt; // clear / keep contents
    std::unique_ptr<QRhiBuffer> post_ubuf; // one aligned PostUniforms slot per pass: downs, ups, composite
    std::array<std::unique_ptr<QRhiShaderResourceBindings>, bloom_mips> down_srb, up_srb;
    std::unique_ptr<QRhiShaderResourceBindings> composite_srb;
    std::unique_ptr<QRhiGraphicsPipeline> down, up, composite;
    std::unique_ptr<QRhiBuffer> ubuf, lights, cube, quad, cone, boxes, walls, beams;
    std::unique_ptr<QRhiTexture> texture;
    std::unique_ptr<QRhiSampler> sampler, comp_sampler; // comp_sampler: mipmapped in cinematic, for blurred reflections
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
            // 8-bit: screen blend, so overlapping beams approach white instead of clipping to it. HDR: a plain sum,
            // which the composite's highlight roll-off brings back into range.
            blend.srcColor = QRhiGraphicsPipeline::One;
            blend.dstColor = hdr ? QRhiGraphicsPipeline::One : QRhiGraphicsPipeline::OneMinusSrcColor;
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

    std::unique_ptr<QRhiGraphicsPipeline> post_pipeline(QRhiRenderPassDescriptor *rp, int sample_count,
                                                        QRhiShaderResourceBindings *layout, const char *frag,
                                                        bool add) {
        std::unique_ptr<QRhiGraphicsPipeline> p(rhi->newGraphicsPipeline());
        p->setShaderStages({{QRhiShaderStage::Vertex, load_shader("post.vert")},
                            {QRhiShaderStage::Fragment, load_shader(QByteArray(frag) + ".frag")}});
        p->setShaderResourceBindings(layout);
        p->setRenderPassDescriptor(rp);
        p->setSampleCount(sample_count);
        if (add) {
            QRhiGraphicsPipeline::TargetBlend blend;
            blend.enable = true;
            blend.srcColor = blend.dstColor = blend.srcAlpha = blend.dstAlpha = QRhiGraphicsPipeline::One;
            p->setTargetBlends({blend});
        }
        if (!p->create())
            qFatal("pipeline %s failed", frag);
        return p;
    }

    quint32 post_slot() const { return quint32(rhi->ubufAligned(sizeof(PostUniforms))); }

    std::unique_ptr<QRhiShaderResourceBindings> post_srb(int slot, QRhiTexture *source, QRhiTexture *bloom = nullptr) {
        using S = QRhiShaderResourceBinding;
        std::unique_ptr<QRhiShaderResourceBindings> srb(rhi->newShaderResourceBindings());
        QVarLengthArray<S, 3> b{S::uniformBuffer(0, S::VertexStage | S::FragmentStage, post_ubuf.get(),
                                                 quint32(slot) * post_slot(), sizeof(PostUniforms)),
                                S::sampledTexture(1, S::FragmentStage, source, sampler.get())};
        if (bloom)
            b.append(S::sampledTexture(2, S::FragmentStage, bloom, sampler.get()));
        srb->setBindings(b.cbegin(), b.cend());
        srb->create();
        return srb;
    }

    // Creates the HDR targets at a placeholder size, plus the bloom pipelines; resize() gives them their real size.
    void build_hdr() {
        const QSize px(16, 16);
        const int msaa_samples =
            rhi->isFeatureSupported(QRhi::MultisampleRenderBuffer) && rhi->supportedSampleCounts().contains(4) ? 4 : 1;
        scene.reset(rhi->newTexture(QRhiTexture::RGBA16F, px, 1, QRhiTexture::RenderTarget));
        scene->create();
        QRhiColorAttachment colour(scene.get());
        if (msaa_samples > 1) {
            msaa.reset(rhi->newRenderBuffer(QRhiRenderBuffer::Color, px, msaa_samples, {}, QRhiTexture::RGBA16F));
            msaa->create();
            colour = QRhiColorAttachment(msaa.get());
            colour.setResolveTexture(scene.get());
        }
        depth.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, px, msaa_samples));
        depth->create();
        QRhiTextureRenderTargetDescription desc(colour);
        desc.setDepthStencilBuffer(depth.get());
        scene_rt.reset(rhi->newTextureRenderTarget(desc));
        scene_rp.reset(scene_rt->newCompatibleRenderPassDescriptor());
        scene_rt->setRenderPassDescriptor(scene_rp.get());
        scene_rt->create();
        for (int i = 0; i < bloom_mips; ++i) {
            mips[i].reset(rhi->newTexture(QRhiTexture::RGBA16F, px, 1, QRhiTexture::RenderTarget));
            mips[i]->create();
            mip_rt[i].reset(rhi->newTextureRenderTarget({QRhiColorAttachment(mips[i].get())}));
            mip_add_rt[i].reset(rhi->newTextureRenderTarget({QRhiColorAttachment(mips[i].get())},
                                                            QRhiTextureRenderTarget::PreserveColorContents));
            if (!mip_rp) {
                mip_rp.reset(mip_rt[i]->newCompatibleRenderPassDescriptor());
                mip_add_rp.reset(mip_add_rt[i]->newCompatibleRenderPassDescriptor());
            }
            mip_rt[i]->setRenderPassDescriptor(mip_rp.get());
            mip_add_rt[i]->setRenderPassDescriptor(mip_add_rp.get());
            mip_rt[i]->create();
            mip_add_rt[i]->create();
        }
        post_ubuf.reset(
            rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, post_slot() * (2 * bloom_mips + 1)));
        post_ubuf->create();
        for (int i = 0; i < bloom_mips; ++i) {
            down_srb[i] = post_srb(i, i == 0 ? scene.get() : mips[i - 1].get());
            if (i > 0) // up pass i reads mip i and adds onto mip i - 1
                up_srb[i] = post_srb(bloom_mips + i, mips[i].get());
        }
        composite_srb = post_srb(2 * bloom_mips, scene.get(), mips[0].get());
        down = post_pipeline(mip_rp.get(), 1, down_srb[0].get(), "bloom_down", false);
        up = post_pipeline(mip_add_rp.get(), 1, up_srb[1].get(), "bloom_up", true);
        size = px;
    }

    // Recreating attachments in place keeps the render targets, pass descriptors and SRBs valid.
    void resize(QSize s) {
        if (s == size)
            return;
        size = s;
        for (QRhiRenderBuffer *b : {msaa.get(), depth.get()})
            if (b) {
                b->setPixelSize(s);
                b->create();
            }
        scene->setPixelSize(s);
        scene->create();
        scene_rt->create();
        for (int i = 0; i < bloom_mips; ++i) {
            mips[i]->setPixelSize({std::max(s.width() >> (i + 1), 1), std::max(s.height() >> (i + 1), 1)});
            mips[i]->create();
            mip_rt[i]->create();
            mip_add_rt[i]->create();
        }
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
    if (!gpu || gpu->rhi != rhi || gpu->cinematic != cinematic) {
        gpu = std::make_unique<Gpu>();
        gpu->rhi = rhi;
        gpu->cinematic = cinematic;
        scene_dirty = composition_dirty = true;
    }
    Gpu &g = *gpu;
    const QVector<quint32> format = rt->renderPassDescriptor()->serializedFormat();
    const bool want_hdr = (hdr || cinematic) && rhi->isTextureFormatSupported(QRhiTexture::RGBA16F);
    if (g.grid && format == g.format && rt->sampleCount() == g.target_samples && want_hdr == g.hdr)
        return;
    g.format = format;
    g.target_samples = rt->sampleCount();
    g.hdr = want_hdr;
    if (!g.ubuf) {
        g.ubuf.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                    quint32(rhi->ubufAligned(sizeof(FrameUniforms)))));
        g.ubuf->create();
        g.sampler.reset(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                        QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        g.sampler->create();
        g.comp_sampler.reset(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                             g.cinematic ? QRhiSampler::Linear : QRhiSampler::None,
                                             QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        g.comp_sampler->create();
        g.texture.reset(rhi->newTexture(QRhiTexture::RGBA8, {16, 16}, 1,
                                        g.cinematic ? QRhiTexture::MipMapped | QRhiTexture::UsedWithGenerateMips
                                                    : QRhiTexture::Flags()));
        g.texture->create();
        g.lights.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                      quint32(rhi->ubufAligned(sizeof(LightUniforms)))));
        g.lights->create();
        using S = QRhiShaderResourceBinding;
        g.srb.reset(rhi->newShaderResourceBindings());
        g.srb->setBindings({S::uniformBuffer(0, S::VertexStage | S::FragmentStage, g.ubuf.get()),
                            S::sampledTexture(1, S::FragmentStage, g.texture.get(), g.comp_sampler.get()),
                            S::uniformBuffer(2, S::FragmentStage, g.lights.get())});
        g.srb->create();
    }
    if (!g.hdr) {
        g.samples = rt->sampleCount();
        g.build_pipelines(rt->renderPassDescriptor());
        return;
    }
    if (!g.scene)
        g.build_hdr();
    g.samples = g.msaa ? g.msaa->sampleCount() : 1;
    g.build_pipelines(g.scene_rp.get());
    g.composite =
        g.post_pipeline(rt->renderPassDescriptor(), rt->sampleCount(), g.composite_srb.get(), "composite", false);
}

bool SceneRenderer::hdr_active() const { return gpu && gpu->hdr; }

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
        if (g.cinematic) {
            u->generateMips(g.texture.get());
            g.small = composition.scaled(160, 90, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
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
    if (g.hdr) {
        g.resize(size);
        const float flip = rhi->isYUpInNDC() != rhi->isYUpInFramebuffer() ? 1.f : 0.f;
        const auto slot = [&](int i, QSize source, float first) {
            const PostUniforms p{{1.f / float(source.width()), 1.f / float(source.height()), flip, first},
                                 {g.cinematic ? cinematic_bloom : bloom_strength, float(std::fmod(time, 1000.0)),
                                  bloom_threshold, g.cinematic ? 1 + scene.haze : 0}};
            u->updateDynamicBuffer(g.post_ubuf.get(), quint32(i) * g.post_slot(), sizeof p, &p);
        };
        for (int i = 0; i < bloom_mips; ++i) {
            slot(i, i == 0 ? size : g.mips[i - 1]->pixelSize(), i == 0 ? 1.f : 0.f);
            slot(bloom_mips + i, g.mips[i]->pixelSize(), 0);
        }
        slot(2 * bloom_mips, size, 0);
    }
    FrameUniforms f{};
    put(f.view_proj, rhi->clipSpaceCorrMatrix() * camera.projection(float(size.width()) / std::max(size.height(), 1)) *
                         camera.view());
    const QVector3D eye = camera.eye(), light = QVector3D(0.4f, 1.0f, 0.6f).normalized();
    std::copy_n(std::array{eye.x(), eye.y(), eye.z(), float(time)}.data(), 4, f.eye);
    std::copy_n(std::array{light.x(), light.y(), light.z(), g.hdr ? (g.cinematic ? 2.f : 1.f) : 0.f}.data(), 4,
                f.light);
    const int spots = int(std::min<std::size_t>(scene.spots.size(), 32));
    std::copy_n(std::array{scene.venue_width / 2, scene.venue_depth / 2, scene.haze, float(spots)}.data(), 4,
                f.floor_info);
    for (int i = 0; i < spots; ++i) {
        const auto &s = scene.spots[std::size_t(i)];
        std::copy_n(std::array{s.centre.x(), s.centre.y(), s.radius, 0.f}.data(), 4, f.spot_pos[i]);
        std::copy_n(std::array{s.colour.x(), s.colour.y(), s.colour.z(), 0.f}.data(), 4, f.spot_col[i]);
    }
    u->updateDynamicBuffer(g.ubuf.get(), 0, sizeof f, &f);
    if (g.cinematic) {
        const LightUniforms l = rig_lights(scene, g.small, g.texture->pixelSize().width());
        u->updateDynamicBuffer(g.lights.get(), 0, sizeof l, &l);
    }

    cb->beginPass(g.hdr ? g.scene_rt.get() : rt, clear_color, {1.0f, 0}, u);
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
    if (!g.hdr)
        return;
    const auto post = [&](QRhiRenderTarget *target, QRhiGraphicsPipeline *p, QRhiShaderResourceBindings *srb) {
        cb->beginPass(target, Qt::black, {1.0f, 0});
        cb->setGraphicsPipeline(p);
        cb->setShaderResources(srb);
        const QSize px = target->pixelSize();
        cb->setViewport({0, 0, float(px.width()), float(px.height())});
        cb->draw(3);
        cb->endPass();
    };
    for (int i = 0; i < bloom_mips; ++i)
        post(g.mip_rt[i].get(), g.down.get(), g.down_srb[i].get());
    for (int i = bloom_mips - 1; i > 0; --i)
        post(g.mip_add_rt[i - 1].get(), g.up.get(), g.up_srb[i].get());
    post(rt, g.composite.get(), g.composite_srb.get());
}

} // namespace sv::render
