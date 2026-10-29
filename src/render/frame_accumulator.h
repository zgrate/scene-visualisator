#pragma once
// The offline render's sub-frame sum (motion blur) and downscale, on the GPU: each sub-frame is drawn into `source`
// (any size, usually 2x the output) and added into an output-sized RGBA16F target with its weight, so only the sum is
// read back. Falls back to RGBA8 where RGBA16F cannot be a render target.
#include <QImage>
#include <QSize>
#include <memory>

class QRhi;
class QRhiCommandBuffer;
class QRhiTexture;
struct QRhiReadbackResult;

namespace sv::render {

class FrameAccumulator {
  public:
    FrameAccumulator();
    ~FrameAccumulator();
    QString create(QRhi *rhi, QRhiTexture *source, QSize out); // empty on success
    // Records one pass: the target becomes source × weight when `first`, else gains it.
    void add(QRhiCommandBuffer *cb, float weight, bool first);
    void read(QRhiCommandBuffer *cb, QRhiReadbackResult *result); // queues the readback of the target
    QImage image(const QRhiReadbackResult &result) const;         // RGBA8888, top row first

  private:
    struct Gpu;
    std::unique_ptr<Gpu> g_;
};

} // namespace sv::render
