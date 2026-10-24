#include "media/media_source.h"

#include "media/native_receiver.h"
#include "media/ndi.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QMediaFormat>
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
    player_.setLoops(loop ? QMediaPlayer::Infinite : 1);
    player_.setPlaybackRate(rate > 0 ? rate : 1.0);
    if (kind_ == "video")
        playing ? player_.play() : player_.pause();
}

void MediaSource::on_frame(const QVideoFrame &frame) {
    if (!frame.isValid() || kind_.isEmpty())
        return;
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
    rec_session_ = std::make_unique<QMediaCaptureSession>();
    rec_input_ = std::make_unique<QVideoFrameInput>();
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
    connect(recorder_.get(), &QMediaRecorder::errorOccurred, this,
            [this](QMediaRecorder::Error, const QString &e) { error_ = tr("Recording: %1").arg(e); });
    rec_session_->setVideoFrameInput(rec_input_.get());
    rec_session_->setRecorder(recorder_.get());
    recorder_->record();
    rec_clock_.start();
    recording_ = true;
    return recorder_->error() == QMediaRecorder::NoError ? QString() : recorder_->errorString();
}

void MediaSource::stop_recording() {
    recording_ = false;
    if (!recorder_)
        return;
    recorder_->stop(); // finalises the file
    recorder_.reset();
    rec_input_.reset();
    rec_session_.reset();
}

void MediaSource::record_frame(const QImage &image) {
    if (!recorder_ || image.isNull())
        return;
    QVideoFrame frame(image);
    frame.setStartTime(rec_clock_.nsecsElapsed() / 1000);
    rec_input_->sendVideoFrame(frame); // false while the encoder is busy: the frame is dropped, timestamps keep sync
}

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
