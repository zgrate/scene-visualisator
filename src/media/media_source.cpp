#include "media/media_source.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QScreen>
#include <QUrl>
#include <QVideoFrame>

namespace sv::media {

MediaSource::MediaSource(QObject *parent) : QObject(parent) {
    player_.setVideoSink(&sink_);
    capture_.setVideoSink(&capture_sink_); // a sink belongs to one source
    capture_.setScreenCapture(&screen_);
    capture_.setWindowCapture(&window_);
    connect(&sink_, &QVideoSink::videoFrameChanged, this, &MediaSource::on_frame);
    connect(&capture_sink_, &QVideoSink::videoFrameChanged, this, &MediaSource::on_frame);
    connect(&player_, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString &e) { error_ = e; });
    connect(&screen_, &QScreenCapture::errorOccurred, this,
            [this](QScreenCapture::Error, const QString &e) { error_ = e; });
    connect(&window_, &QWindowCapture::errorOccurred, this,
            [this](QWindowCapture::Error, const QString &e) { error_ = e; });
}

void MediaSource::open(const QString &kind, const QString &target) {
    player_.stop();
    screen_.setActive(false);
    window_.setActive(false);
    error_.clear();
    {
        QMutexLocker lock(&mutex_);
        image_ = {};
        ++serial_;
        fps_ = 0;
        last_ms_ = 0;
    }
    kind_.clear();
    if (kind == "video" && !target.isEmpty()) {
        player_.setSource(QUrl::fromLocalFile(target));
        player_.play();
    } else if (kind == "screen") {
        const auto screens = QGuiApplication::screens();
        auto it = std::ranges::find_if(screens, [&](QScreen *s) { return s->name() == target; });
        screen_.setScreen(it == screens.end() ? QGuiApplication::primaryScreen() : *it);
        screen_.setActive(true);
    } else if (kind == "window") {
        const auto windows = QWindowCapture::capturableWindows();
        auto it = std::ranges::find_if(windows, [&](const QCapturableWindow &w) { return w.description() == target; });
        if (it == windows.end()) {
            error_ = tr("Window not found: %1").arg(target);
            return;
        }
        window_.setWindow(*it);
        window_.setActive(true);
    } else {
        return;
    }
    kind_ = kind;
}

void MediaSource::set_playback(bool playing, bool loop, double rate) {
    player_.setLoops(loop ? QMediaPlayer::Infinite : 1);
    player_.setPlaybackRate(rate > 0 ? rate : 1.0);
    if (kind_ == "video")
        playing ? player_.play() : player_.pause();
}

void MediaSource::on_frame(const QVideoFrame &frame) {
    if (!frame.isValid() || kind_.isEmpty())
        return;
    QImage image = frame.toImage(); // shortcut: CPU copy per frame; share GPU textures when B2 lands (NFR-3)
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QMutexLocker lock(&mutex_);
    if (last_ms_ && now > last_ms_)
        fps_ = fps_ ? fps_ * 0.9 + 100.0 / double(now - last_ms_) : 1000.0 / double(now - last_ms_);
    last_ms_ = now;
    image_ = std::move(image);
    ++serial_;
}

QImage MediaSource::latest(quint64 &serial) const {
    QMutexLocker lock(&mutex_);
    if (serial == serial_)
        return {};
    serial = serial_;
    return image_;
}

MediaStatus MediaSource::status() const {
    QMutexLocker lock(&mutex_);
    const bool fresh = last_ms_ && QDateTime::currentMSecsSinceEpoch() - last_ms_ < 1000;
    return {image_.size(), fresh ? fps_ : 0.0, fresh, error_};
}

QStringList MediaSource::available(const QString &kind) {
    QStringList out;
    if (kind == "screen")
        for (QScreen *s : QGuiApplication::screens())
            out << s->name();
    else if (kind == "window")
        for (const QCapturableWindow &w : QWindowCapture::capturableWindows())
            out << w.description();
    return out;
}

} // namespace sv::media
