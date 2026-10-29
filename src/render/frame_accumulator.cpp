#include "render/frame_accumulator.h"

#include <QFile>
#include <rhi/qrhi.h>

namespace sv::render {

namespace {
struct PostUniforms { // the std140 Post block of post.vert and accumulate.frag
    float texel[4], params[4];
};

QShader load_shader(const char *name) {
    QFile f(QStringLiteral(":/sv/shaders/%1.qsb").arg(QLatin1StringView(name)));
    if (!f.open(QIODevice::ReadOnly))
        qFatal("missing shader %s", name); // build error, not a runtime condition
    return QShader::fromSerialized(f.readAll());
}
} // namespace

struct FrameAccumulator::Gpu {
    QRhi *rhi = nullptr;
    bool half = false; // RGBA16F target
    std::unique_ptr<QRhiTexture> target;
    std::unique_ptr<QRhiTextureRenderTarget> rt;
    std::unique_ptr<QRhiRenderPassDescriptor> rp;
    std::unique_ptr<QRhiBuffer> ubuf;
    std::unique_ptr<QRhiSampler> sampler;
    std::unique_ptr<QRhiShaderResourceBindings> srb;
    std::unique_ptr<QRhiGraphicsPipeline> replace, add; // first sub-frame overwrites, the rest blend One/One
};

FrameAccumulator::FrameAccumulator() = default;
FrameAccumulator::~FrameAccumulator() = default;

QString FrameAccumulator::create(QRhi *rhi, QRhiTexture *source, QSize out) {
    auto g = std::make_unique<Gpu>();
    g->rhi = rhi;
    g->half = rhi->isTextureFormatSupported(QRhiTexture::RGBA16F);
    g->target.reset(rhi->newTexture(g->half ? QRhiTexture::RGBA16F : QRhiTexture::RGBA8, out, 1,
                                    QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!g->target->create())
        return QStringLiteral("Cannot make the %1×%2 accumulation target").arg(out.width()).arg(out.height());
    // Preserved: every sub-frame after the first adds to what is there.
    g->rt.reset(rhi->newTextureRenderTarget({QRhiColorAttachment(g->target.get())},
                                            QRhiTextureRenderTarget::PreserveColorContents));
    g->rp.reset(g->rt->newCompatibleRenderPassDescriptor());
    g->rt->setRenderPassDescriptor(g->rp.get());
    g->ubuf.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(PostUniforms)));
    g->sampler.reset(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                     QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    if (!g->rt->create() || !g->ubuf->create() || !g->sampler->create())
        return QStringLiteral("Cannot make the accumulation pass");
    using S = QRhiShaderResourceBinding;
    g->srb.reset(rhi->newShaderResourceBindings());
    g->srb->setBindings({S::uniformBuffer(0, S::VertexStage | S::FragmentStage, g->ubuf.get()),
                         S::sampledTexture(1, S::FragmentStage, source, g->sampler.get())});
    g->srb->create();
    for (bool blend : {false, true}) {
        std::unique_ptr<QRhiGraphicsPipeline> p(rhi->newGraphicsPipeline());
        p->setShaderStages({{QRhiShaderStage::Vertex, load_shader("post.vert")},
                            {QRhiShaderStage::Fragment, load_shader("accumulate.frag")}});
        p->setShaderResourceBindings(g->srb.get());
        p->setRenderPassDescriptor(g->rp.get());
        if (blend) {
            QRhiGraphicsPipeline::TargetBlend b;
            b.enable = true;
            b.srcColor = b.dstColor = b.srcAlpha = b.dstAlpha = QRhiGraphicsPipeline::One;
            p->setTargetBlends({b});
        }
        if (!p->create())
            return QStringLiteral("Cannot make the accumulation pipeline");
        (blend ? g->add : g->replace) = std::move(p);
    }
    g_ = std::move(g);
    return {};
}

void FrameAccumulator::add(QRhiCommandBuffer *cb, float weight, bool first) {
    QRhi *rhi = g_->rhi;
    const float flip = rhi->isYUpInNDC() != rhi->isYUpInFramebuffer() ? 1.f : 0.f;
    const PostUniforms p{{0, 0, flip, 0}, {weight, 0, 0, 0}};
    QRhiResourceUpdateBatch *u = rhi->nextResourceUpdateBatch();
    u->updateDynamicBuffer(g_->ubuf.get(), 0, sizeof p, &p);
    cb->beginPass(g_->rt.get(), Qt::black, {1.0f, 0}, u);
    cb->setGraphicsPipeline((first ? g_->replace : g_->add).get());
    const QSize size = g_->target->pixelSize();
    cb->setViewport({0, 0, float(size.width()), float(size.height())});
    cb->setShaderResources();
    cb->draw(3);
    cb->endPass();
}

void FrameAccumulator::read(QRhiCommandBuffer *cb, QRhiReadbackResult *result) {
    QRhiResourceUpdateBatch *u = g_->rhi->nextResourceUpdateBatch();
    u->readBackTexture({g_->target.get()}, result);
    cb->resourceUpdate(u);
}

QImage FrameAccumulator::image(const QRhiReadbackResult &r) const {
    const QImage wrap(reinterpret_cast<const uchar *>(r.data.constData()), r.pixelSize.width(), r.pixelSize.height(),
                      g_->half ? QImage::Format_RGBA16FPx4 : QImage::Format_RGBA8888);
    const QImage image = g_->half ? wrap.convertToFormat(QImage::Format_RGBA8888) : wrap.copy(); // r.data is reused
    return g_->rhi->isYUpInFramebuffer() ? image.flipped() : image;
}

} // namespace sv::render
