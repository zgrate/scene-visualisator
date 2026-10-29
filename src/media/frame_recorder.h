#pragma once
// Writes QImages to an H.264 MP4 through QVideoFrameInput and QMediaRecorder. Use it on the thread that owns it
// (the UI thread); producers on other threads post their frames there.
#include <QElapsedTimer>
#include <QImage>
#include <QMediaCaptureSession>
#include <QMediaRecorder>
#include <QVideoFrame>
#include <QVideoFrameInput>

#include <deque>
#include <functional>
#include <memory>

namespace sv::media {

class FrameRecorder {
  public:
    // lossless picks near-lossless (150 Mbit/s constant bitrate) over the encoder's high quality. on_error hears
    // encoder errors that come later. Returns an error, empty on success.
    QString start(const QString &path, bool lossless, std::function<void(const QString &)> on_error = {});
    void stop();                    // finalises the file
    void push(const QImage &image); // stamped with the time since start(); dropped while the encoder is busy
    // Offline render: stamped with `us` and queued while the encoder is busy, never dropped.
    void push(const QImage &image, qint64 us);
    std::size_t queued() const { return queue_.size(); }
    bool active() const { return bool(recorder_); }

  private:
    void drain();

    std::unique_ptr<QMediaCaptureSession> session_;
    std::unique_ptr<QVideoFrameInput> input_;
    std::unique_ptr<QMediaRecorder> recorder_;
    QElapsedTimer clock_;
    std::deque<QVideoFrame> queue_;
};

} // namespace sv::media
