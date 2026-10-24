#pragma once
// FR-14 live composition sources: video files, screen and window capture through QtMultimedia. Lives on the UI
// thread; frames land in a mutex-guarded latest-wins slot that the render thread polls by serial.
#include <QImage>
#include <QMediaCaptureSession>
#include <QElapsedTimer>
#include <QMediaPlayer>
#include <QMediaRecorder>
#include <QVideoFrameInput>
#include <QMutex>
#include <QObject>
#include <QScreenCapture>
#include <QVideoSink>
#include <QWindowCapture>

#include <atomic>
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
    qint64 position_ms() const { return player_.position(); } // video only
    void seek(qint64 ms) { player_.setPosition(ms); }           // video only

    // Records every frame that arrives from now on to an H.264 MP4 at `path`. lossless picks near-lossless
    // (150 Mbit/s constant bitrate) over the encoder's high quality. Returns an error, empty on success.
    QString start_recording(const QString &path, bool lossless);
    void stop_recording();

    // Render-thread safe. The latest frame when newer than `serial` (which is then advanced), else a null image.
    QImage latest(quint64 &serial) const;
    quint64 serial() const; // any thread; advances with every frame, without taking it
    MediaStatus status() const;

    // What `open` accepts for a kind: screen names, window titles or sender names; empty for other kinds. NDI blocks
    // for a second of discovery (NFR-9: only on request).
    static QStringList available(const QString &kind);

  private:
    void on_frame(const QVideoFrame &frame);
    void store(QImage image); // any thread
    void record_frame(const QImage &image);

    QString kind_;
    QMediaPlayer player_;
    QMediaCaptureSession capture_;
    QScreenCapture screen_;
    QWindowCapture window_;
    QVideoSink sink_, capture_sink_;
    QString error_;
    std::unique_ptr<NdiReceiver> ndi_;
    std::unique_ptr<NativeReceiver> native_; // Spout or Syphon
    std::unique_ptr<QMediaCaptureSession> rec_session_;
    std::unique_ptr<QVideoFrameInput> rec_input_;
    std::unique_ptr<QMediaRecorder> recorder_;
    QElapsedTimer rec_clock_;
    std::atomic_bool recording_{false}; // read by store() on receiver threads

    mutable QMutex mutex_;
    QImage image_;
    quint64 serial_ = 0;
    qint64 last_ms_ = 0;
    double fps_ = 0;
    qint64 arrived_ms_ = 0;
    mutable double latency_ms_ = 0;
};

} // namespace sv::media
