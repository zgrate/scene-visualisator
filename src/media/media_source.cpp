#include "media/media_source.h"

#include "media/native_receiver.h"
#include "media/ndi.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QScreen>
#include <QUrl>
#include <QVideoFrame>

#include <cmath>

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
    connect(&player_, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus st) {
        if (st == QMediaPlayer::EndOfMedia)
            ended_ = true;
    });
    connect(&screen_, &QScreenCapture::errorOccurred, this,
            [this](QScreenCapture::Error, const QString &e) { error_ = e; });
    connect(&window_, &QWindowCapture::errorOccurred, this,
            [this](QWindowCapture::Error, const QString &e) { error_ = e; });
}

MediaSource::~MediaSource() = default;

void MediaSource::open(const QString &kind, const QString &target) {
    player_.stop();
    screen_.setActive(false);
    window_.setActive(false);
    ndi_.reset(); // joins the receiver thread before the slot below is reset
    native_.reset();
    error_.clear();
    {
        QMutexLocker lock(&mutex_);
        image_ = {};
        ++serial_;
        fps_ = 0;
        last_ms_ = 0;
        latency_ms_ = 0;
        frame_start_ = frame_end_ = -1;
    }
    kind_.clear();
    stepping_ = false, queue_.clear();
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
    } else if (kind == "ndi" && !target.isEmpty()) {
        if (!ndi_api()) {
            error_ = tr("NDI runtime not installed (ndi.video/tools)");
            return;
        }
        ndi_ = std::make_unique<NdiReceiver>(target, [this](QImage image) { store(std::move(image)); });
    } else if ((kind == "spout" || kind == "syphon") && !target.isEmpty()) {
        native_ = NativeReceiver::create(kind, target, [this](QImage image) { store(std::move(image)); });
        if (!native_) {
            error_ = tr("%1 is not available on this system").arg(kind);
            return;
        }
    } else {
        return;
    }
    kind_ = kind;
}

void MediaSource::set_playback(bool playing, bool loop, double rate) {
    stepping_ = false, queue_.clear();
    player_.setLoops(loop ? QMediaPlayer::Infinite : 1);
    player_.setPlaybackRate(rate > 0 ? rate : 1.0);
    if (kind_ == "video")
        playing ? player_.play() : player_.pause();
}

void MediaSource::begin_stepping(qint64 from_ms) {
    queue_.clear();
    gaps_ = 0, ended_ = false, shown_start_ = -2, stepping_ = true;
    seeked_ = false, seek_ms_ = double(from_ms), seek_clock_.start();
    player_.setLoops(1);
    player_.setPlaybackRate(2.0); // decodes ahead of the render; the queue bound stops it running away
    player_.setPosition(from_ms);
    player_.play();
}

void MediaSource::end_stepping() {
    stepping_ = false;
    queue_.clear();
    player_.setPlaybackRate(1.0);
}

bool MediaSource::shows(double t) {
    if (!stepping_) {
        const auto [from, to] = frame_span();
        return from >= 0 && from <= t + 1 && (to > t || to <= from); // the frame on screen at t
    }
    while (queue_.size() > 1 && queue_[1].start_ms <= t + 1) // older frames are done with
        queue_.pop_front();
    if (queue_.empty()) {
        if (!seeked_ && seek_clock_.elapsed() > 1000) // a seek sent before the media was ready is dropped
            player_.setPosition(qint64(seek_ms_)), seek_clock_.restart();
        return ended_; // past the end: hold the last frame
    }
    const Queued &q = queue_.front();
    if (queue_.size() < 6 && player_.playbackState() == QMediaPlayer::PausedState)
        player_.play();
    if (q.end_ms > q.start_ms && q.end_ms <= t && !ended_)
        return false; // the head is stale and the next frame has not arrived yet
    // Nothing covers t yet and the decoder has nothing earlier: a skipped frame. Use the head rather than stall.
    if (q.start_ms > t + 1 && !ended_ && shown_start_ >= 0 && q.start_ms > shown_start_ + 1 + 1000.0 / 30)
        ++gaps_;
    if (q.start_ms != shown_start_) {
        {
            QMutexLocker lock(&mutex_);
            frame_start_ = q.start_ms, frame_end_ = q.end_ms;
        }
        store(q.image);
        shown_start_ = q.start_ms;
    }
    return true;
}

void MediaSource::on_frame(const QVideoFrame &frame) {
    if (!frame.isValid() || kind_.isEmpty())
        return;
    if (stepping_) { // converted now, so the decoder's surface pool is never held by the queue
        // Frames decoded before the seek took effect still arrive, with their old times: a frame ahead of the
        // target would be shown and hold until t reached it. Wait for one within a second of the target.
        if (!seeked_ && !(seeked_ = std::abs(double(frame.startTime()) / 1000 - seek_ms_) <= 1000))
            return;
        queue_.push_back({double(frame.startTime()) / 1000, double(frame.endTime()) / 1000, frame.toImage()});
        if (queue_.size() >= 16)
            player_.pause();
        return;
    }
    {
        QMutexLocker lock(&mutex_);
        frame_start_ = double(frame.startTime()) / 1000;
        frame_end_ = double(frame.endTime()) / 1000;
    }
    store(frame.toImage()); // shortcut: CPU copy per frame; share GPU textures if NFR-3 latency fails on hardware
}

void MediaSource::store(QImage image) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QMutexLocker lock(&mutex_);
    if (last_ms_ && now > last_ms_)
        fps_ = fps_ ? fps_ * 0.9 + 100.0 / double(now - last_ms_) : 1000.0 / double(now - last_ms_);
    last_ms_ = now;
    arrived_ms_ = now;
    image_ = image;
    ++serial_;
    lock.unlock();
    if (recording_) // the recorder lives on the UI thread
        QMetaObject::invokeMethod(this, [this, image = std::move(image)] { record_frame(image); });
}

QString MediaSource::start_recording(const QString &path, bool lossless) {
    stop_recording();
    const QString error =
        recorder_.start(path, lossless, [this](const QString &e) { error_ = tr("Recording: %1").arg(e); });
    recording_ = true;
    return error;
}

void MediaSource::stop_recording() {
    recording_ = false;
    recorder_.stop();
}

void MediaSource::record_frame(const QImage &image) { recorder_.push(image); }

quint64 MediaSource::serial() const {
    QMutexLocker lock(&mutex_);
    return serial_;
}

QImage MediaSource::latest(quint64 &serial) const {
    QMutexLocker lock(&mutex_);
    if (serial == serial_)
        return {};
    serial = serial_;
    if (arrived_ms_) {
        const double age = double(QDateTime::currentMSecsSinceEpoch() - arrived_ms_);
        latency_ms_ = latency_ms_ ? latency_ms_ * 0.9 + age * 0.1 : age;
    }
    return image_;
}

std::pair<double, double> MediaSource::frame_span() const {
    QMutexLocker lock(&mutex_);
    return {frame_start_, frame_end_};
}

MediaStatus MediaSource::status() const {
    QMutexLocker lock(&mutex_);
    const bool fresh = last_ms_ && QDateTime::currentMSecsSinceEpoch() - last_ms_ < 1000;
    return {image_.size(), fresh ? fps_ : 0.0, fresh, fresh ? latency_ms_ : 0.0, error_};
}

QStringList MediaSource::available(const QString &kind) {
    QStringList out;
    if (kind == "screen")
        for (QScreen *s : QGuiApplication::screens())
            out << s->name();
    else if (kind == "window")
        for (const QCapturableWindow &w : QWindowCapture::capturableWindows())
            out << w.description();
    else if (kind == "ndi")
        out = ndi_sources();
    else if (kind == "spout" || kind == "syphon")
        out = NativeReceiver::senders(kind);
    return out;
}

} // namespace sv::media
