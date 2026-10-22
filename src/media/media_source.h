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

namespace sv::media {

struct MediaStatus {
    QSize size;
    double fps = 0;
    bool signal = false; // a frame arrived within the last second
    QString error;
};

class MediaSource : public QObject {
    Q_OBJECT
  public:
    explicit MediaSource(QObject *parent = nullptr);

    // kind: video (target = local file), screen (screen name), window (window title); anything else stops.
    void open(const QString &kind, const QString &target);
    void set_playback(bool playing, bool loop, double rate); // video only
    bool active() const { return !kind_.isEmpty(); }

    // Render-thread safe. The latest frame when newer than `serial` (which is then advanced), else a null image.
    QImage latest(quint64 &serial) const;
    MediaStatus status() const;

    // What `open` accepts for a kind: screen names or capturable window titles; empty for other kinds.
    static QStringList available(const QString &kind);

  private:
    void on_frame(const QVideoFrame &frame);

    QString kind_;
    QMediaPlayer player_;
    QMediaCaptureSession capture_;
    QScreenCapture screen_;
    QWindowCapture window_;
    QVideoSink sink_, capture_sink_;
    QString error_;

    mutable QMutex mutex_;
    QImage image_;
    quint64 serial_ = 0;
    qint64 last_ms_ = 0;
    double fps_ = 0;
};

} // namespace sv::media
