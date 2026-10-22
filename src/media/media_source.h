#pragma once
// FR-14 live composition sources: video files, screen and window capture through QtMultimedia. Lives on the UI
// thread; frames land in a mutex-guarded latest-wins slot that the render thread polls by serial.
#include <QImage>
#include <QMediaCaptureSession>
#include <QMediaPlayer>
#include <QMutex>
#include <QObject>
#include <QScreenCapture>
#include <QVideoSink>
#include <QWindowCapture>

#include <memory>

namespace sv::media {

class NdiReceiver;
class NativeReceiver;

struct MediaStatus {
    QSize size;
    double fps = 0;
    bool signal = false;   // a frame arrived within the last second
    double latency_ms = 0; // NFR-3: frame arrival to the render thread picking it up, smoothed
    QString error;
};

class MediaSource : public QObject {
    Q_OBJECT
  public:
    explicit MediaSource(QObject *parent = nullptr);
    ~MediaSource() override;

    // kind: video (target = local file), screen (screen name), window (window title), ndi, spout or syphon (sender
    // name); anything else stops.
    void open(const QString &kind, const QString &target);
    void set_playback(bool playing, bool loop, double rate); // video only
    bool active() const { return !kind_.isEmpty(); }

    // Render-thread safe. The latest frame when newer than `serial` (which is then advanced), else a null image.
    QImage latest(quint64 &serial) const;
    MediaStatus status() const;

    // What `open` accepts for a kind: screen names, window titles or sender names; empty for other kinds. NDI blocks
    // for a second of discovery (NFR-9: only on request).
    static QStringList available(const QString &kind);

  private:
    void on_frame(const QVideoFrame &frame);
    void store(QImage image); // any thread

    QString kind_;
    QMediaPlayer player_;
    QMediaCaptureSession capture_;
    QScreenCapture screen_;
    QWindowCapture window_;
    QVideoSink sink_, capture_sink_;
    QString error_;
    std::unique_ptr<NdiReceiver> ndi_;
    std::unique_ptr<NativeReceiver> native_; // Spout or Syphon

    mutable QMutex mutex_;
    QImage image_;
    quint64 serial_ = 0;
    qint64 last_ms_ = 0;
    double fps_ = 0;
    qint64 arrived_ms_ = 0;
    mutable double latency_ms_ = 0;
};

} // namespace sv::media
