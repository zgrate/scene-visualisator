#include "media/frame_recorder.h"

#include <QMediaFormat>
#include <QUrl>
#include <QVideoFrame>

namespace sv::media {

QString FrameRecorder::start(const QString &path, bool lossless, std::function<void(const QString &)> on_error) {
    stop();
    session_ = std::make_unique<QMediaCaptureSession>();
    input_ = std::make_unique<QVideoFrameInput>();
    recorder_ = std::make_unique<QMediaRecorder>();
    QMediaFormat format(QMediaFormat::MPEG4);
    format.setVideoCodec(QMediaFormat::VideoCodec::H264);
    recorder_->setMediaFormat(format);
    recorder_->setOutputLocation(QUrl::fromLocalFile(path));
    if (lossless) {
        recorder_->setQuality(QMediaRecorder::VeryHighQuality);
        recorder_->setEncodingMode(QMediaRecorder::ConstantBitRateEncoding);
        recorder_->setVideoBitRate(150'000'000);
    } else {
        recorder_->setQuality(QMediaRecorder::HighQuality);
    }
    if (on_error)
        QObject::connect(recorder_.get(), &QMediaRecorder::errorOccurred, recorder_.get(),
                         [on_error](QMediaRecorder::Error, const QString &e) { on_error(e); });
    QObject::connect(input_.get(), &QVideoFrameInput::readyToSendVideoFrame, input_.get(), [this] { drain(); });
    session_->setVideoFrameInput(input_.get());
    session_->setRecorder(recorder_.get());
    recorder_->record();
    clock_.start();
    return recorder_->error() == QMediaRecorder::NoError ? QString() : recorder_->errorString();
}

void FrameRecorder::stop() {
    if (!recorder_)
        return;
    drain();
    queue_.clear(); // shortcut: frames the encoder never took are lost on stop; wait for readiness if renders end short
    recorder_->stop();
    recorder_.reset();
    input_.reset();
    session_.reset();
}

void FrameRecorder::push(const QImage &image) {
    if (!recorder_ || image.isNull())
        return;
    QVideoFrame frame(image);
    frame.setStartTime(clock_.nsecsElapsed() / 1000);
    input_->sendVideoFrame(frame); // false while the encoder is busy: the frame is dropped, timestamps keep sync
}

void FrameRecorder::push(const QImage &image, qint64 us) {
    if (!recorder_ || image.isNull())
        return;
    QVideoFrame frame(image);
    frame.setStartTime(us);
    queue_.push_back(std::move(frame));
    drain();
}

void FrameRecorder::drain() {
    while (!queue_.empty() && input_->sendVideoFrame(queue_.front()))
        queue_.pop_front();
}

} // namespace sv::media
