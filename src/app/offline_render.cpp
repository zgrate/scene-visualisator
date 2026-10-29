#include "offline_render.h"

#include "composition_builder.h"
#include "media/media_source.h"
#include "render/frame_accumulator.h"
#include "render/scene_renderer.h"
#include "session.h"

#include <QOffscreenSurface>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>

using sv::json;

json camera_at(const json &keys, double t_ms) {
    if (!keys.is_array() || keys.empty())
        return json::object();
    json a = keys.front(), b = a; // a: the last key at or before t (with every field held), b: the one after
    double ta = a.value("t", 0.0), tb = ta;
    for (const json &k : keys) {
        if (!k.is_object())
            continue;
        json held = b;
        held.update(k);
        b = held, tb = k.value("t", 0.0);
        if (tb > t_ms)
            break;
        a = b, ta = tb;
    }
    a.erase("t"), b.erase("t");
    if (tb <= ta)
        return a;
    const double x = std::clamp((t_ms - ta) / (tb - ta), 0.0, 1.0), e = x * x * (3 - 2 * x);
    const auto mix = [e](const json &u, const json &v) { return u.get<double>() * (1 - e) + v.get<double>() * e; };
    json out = a;
    for (auto &[key, value] : out.items()) {
        if (!b.contains(key))
            continue;
        if (value.is_number())
            value = mix(value, b[key]);
        else if (value.is_array() && b[key].is_array() && value.size() == b[key].size())
            for (std::size_t i = 0; i < value.size(); ++i)
                value[i] = mix(value[i], b[key][i]);
    }
    return out;
}

namespace {
sv::render::OrbitCamera camera_from(const json &state) {
    sv::render::OrbitCamera c;
    if (const json t = state.value("target", json()); t.is_array() && t.size() == 3)
        c.target = QVector3D(t[0].get<float>(), t[1].get<float>(), t[2].get<float>());
    c.yaw = state.value("yaw", c.yaw);
    c.pitch = state.value("pitch", c.pitch);
    c.distance = state.value("distance", c.distance);
    c.fov = state.value("fov", c.fov);
    return c;
}
} // namespace

// One output frame's inputs, gathered on the UI thread: immutable snapshots and copies, nothing the UI may edit.
struct FrameInputs {
    quint64 generation = 0;
    std::shared_ptr<const sv::json> project;
    std::shared_ptr<const sv::render::FixtureLibrary> library;
    std::shared_ptr<const sv::render::MeshLibrary> meshes;
    std::map<std::uint16_t, sv::proto::Slots> frames; // the engine's mutex-guarded copy: the TripleBuffers are the viewport's
    QImage composition;                                // null: unchanged since the last frame
    std::vector<std::pair<sv::render::OrbitCamera, double>> subframes; // camera and scene time (s) per sub-frame
};

// Its own device, not the window's: Qt Quick draws nothing while the window is minimised or the screen is locked.
// Worker thread only: the QRhi is made, used and destroyed there.
struct OfflineRender::View {
    std::unique_ptr<QRhi> rhi;
    std::unique_ptr<QRhiTexture> colour;
    std::unique_ptr<QRhiRenderBuffer> depth;
    std::unique_ptr<QRhiTextureRenderTarget> rt;
    std::unique_ptr<QRhiRenderPassDescriptor> rp;
    sv::render::SceneRenderer scene; // released before the QRhi
    sv::render::FrameAccumulator accumulator;
    QRhiReadbackResult result;

    QString create(QSize size, QSize out, bool cinematic, bool hdr, QOffscreenSurface *surface) {
#if defined(Q_OS_WIN)
        QRhiD3D11InitParams params;
        rhi.reset(QRhi::create(QRhi::D3D11, &params));
#elif defined(Q_OS_MACOS)
        QRhiMetalInitParams params;
        rhi.reset(QRhi::create(QRhi::Metal, &params));
#else
        QRhiGles2InitParams params;
        params.fallbackSurface = surface;
        rhi.reset(QRhi::create(QRhi::OpenGLES2, &params));
#endif
        if (!rhi)
            return tr("No graphics device to render with");
        colour.reset(rhi->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget));
        depth.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size));
        QRhiTextureRenderTargetDescription desc{QRhiColorAttachment(colour.get())};
        desc.setDepthStencilBuffer(depth.get());
        rt.reset(rhi->newTextureRenderTarget(desc));
        rp.reset(rt->newCompatibleRenderPassDescriptor());
        rt->setRenderPassDescriptor(rp.get());
        if (!colour->create() || !depth->create() || !rt->create())
            return tr("Cannot make a %1×%2 render target").arg(size.width()).arg(size.height());
        if (const QString e = accumulator.create(rhi.get(), colour.get(), out); !e.isEmpty())
            return e;
        scene.hdr = hdr;
        scene.cinematic = cinematic;
        return {};
    }

    // The viewport renderer's frame (viewport_item.cpp) at each sub-frame's scene time, without selection or gizmo,
    // averaged on the GPU and read back once.
    QImage render(const FrameInputs &in, bool *failed, Stage &st) {
        QElapsedTimer clock;
        clock.start();
        if (!in.composition.isNull())
            scene.set_composition(in.composition);
        const auto fixtures = sv::render::decode_fixtures(*in.project, *in.library, [&](std::uint16_t u) {
            auto it = in.frames.find(u);
            return it == in.frames.end() ? std::span<const std::uint8_t>()
                                         : std::span<const std::uint8_t>(it->second);
        });
        const int n = int(in.subframes.size());
        for (int k = 0; k < n; ++k) {
            const auto &[camera, time_s] = in.subframes[k];
            scene.set_scene(sv::render::build_scene(*in.project, fixtures, {}, time_s, *in.meshes));
            scene.set_camera(camera, time_s);
            st.build += double(clock.restart());
            QRhiCommandBuffer *cb = nullptr;
            if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
                return *failed = true, QImage();
            scene.initialize(rhi.get(), rt.get());
            scene.render(cb, rt.get());
            accumulator.add(cb, 1.0f / n, k == 0);
            if (k == n - 1)
                accumulator.read(cb, &result);
            // One frame per sub-frame: SceneRenderer updates its dynamic buffers once per frame.
            if (rhi->endOffscreenFrame() != QRhi::FrameOpSuccess) // waits for the GPU, so the readback is complete
                return *failed = true, QImage();
            st.draw += double(clock.restart());
        }
        QImage out = accumulator.image(result);
        st.readback += double(clock.elapsed());
        return out;
    }
};

OfflineRender::OfflineRender() {
    worker_.setObjectName("sv-offline-render");
    worker_context_.moveToThread(&worker_);
}

OfflineRender::~OfflineRender() {
    shutdown_view();
    worker_.quit();
    worker_.wait();
}

void OfflineRender::shutdown_view() {
    if (worker_.isRunning()) // queued behind a frame in flight, so it waits for it
        QMetaObject::invokeMethod(
            &worker_context_, [this] { view_.reset(); }, Qt::BlockingQueuedConnection);
}

QString OfflineRender::start(Job job, std::function<Inputs(double)> seek, std::function<quint64()> dmx_serial,
                             std::function<void()> done, std::function<double()> ui_ms) {
    if (active())
        return tr("A render is already running");
    if (job.fps <= 0 || job.fps > 240)
        return tr("fps is 1 to 240");
    if (job.duration_ms <= 0)
        return tr("Give a duration: there is no take or video to take it from");
    job.size = QSize(std::clamp(job.size.width(), 16, 7680) & ~1, std::clamp(job.size.height(), 16, 4320) & ~1);
    job.samples = std::clamp(job.samples > 0 ? job.samples : job.cinematic ? 4 : 1, 1, 64);
    // Cinematic draws at 2× (up to 4K out) and scales down: clean truss edges and LED cells, and less march grain.
    const QSize draw_size = job.cinematic && job.supersample && job.size.width() <= 3840 ? job.size * 2 : job.size;
    const bool hdr = Session::preference("renderQuality", "high").toString() != "fast";
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    surface_.reset(QRhiGles2InitParams::newFallbackSurface(QRhiGles2InitParams().format)); // a GUI-thread object
#endif
    if (!worker_.isRunning())
        worker_.start();
    QString error;
    QMetaObject::invokeMethod(
        &worker_context_,
        [&] {
            view_ = std::make_unique<View>();
            error = view_->create(draw_size, job.size, job.cinematic, hdr, surface_.get());
            if (!error.isEmpty())
                return view_.reset();
            backend_ = QString::fromUtf8(view_->rhi->backendName());
            device_ = QString::fromUtf8(view_->rhi->driverInfo().deviceName);
        },
        Qt::BlockingQueuedConnection);
    if (!error.isEmpty())
        return surface_.reset(), error;
    if (const QString e = recorder_.start(job.path, job.lossless,
                                          [this](const QString &e) {
                                              QTimer::singleShot(0, this, [this, e] {
                                                  stop(e);
                                              }); // not inside the recorder's own signal
                                          });
        !e.isEmpty()) {
        recorder_.stop();
        shutdown_view();
        return surface_.reset(), e;
    }
    job_ = std::move(job), seek_ = std::move(seek), dmx_serial_ = std::move(dmx_serial);
    done_ = std::move(done), ui_ms_ = std::move(ui_ms);
    frame_ = 0, frames_ = std::max(1, int(std::floor(job_.duration_ms * job_.fps / 1000)));
    error_.clear();
    composition_ = {}, revision_ = ~0ull;
    stages_.clear(), done_at_.clear(), waited_on_ = nullptr;
    preview_ = {}, preview_serial_ = 0, preview_at_.invalidate(), encoder_.clear();
    started_.start();
    poll_.setInterval(5);
    poll_.setTimerType(Qt::PreciseTimer); // Windows' default timer ticks at 15.6 ms: a frame's worth of waiting
    connect(&poll_, &QTimer::timeout, this, &OfflineRender::poll, Qt::UniqueConnection);
    next();
    return {};
}

void OfflineRender::stop(const QString &error) {
    if (!active())
        return;
    if (!error.isEmpty())
        error_ = error;
    ++generation_; // a frame finishing on the worker after this is dropped
    poll_.stop();
    shutdown_view();
    recorder_.stop();
    surface_.reset();
    if (done_)
        done_();
}

QVariantMap OfflineRender::status() const {
    Stage sum;
    for (const Stage &s : stages_) {
        sum.wait_video += s.wait_video, sum.wait_dmx += s.wait_dmx, sum.wait_encoder += s.wait_encoder;
        sum.build += s.build, sum.draw += s.draw, sum.readback += s.readback, sum.push += s.push;
    }
    const double n = std::max<double>(1, double(stages_.size()));
    const qint64 elapsed = started_.isValid() ? started_.elapsed() : 0;
    double rate = 0; // frames/s over the last 2 s, or over the whole run until two frames sit in that window
    const auto recent = std::ranges::find_if(done_at_, [&](qint64 t) { return t >= elapsed - 2000; });
    if (const auto n_recent = done_at_.end() - recent; n_recent >= 2)
        rate = double(n_recent - 1) * 1000.0 / double(std::max<qint64>(1, done_at_.back() - *recent));
    else if (frame_ > 0 && elapsed > 0)
        rate = frame_ * 1000.0 / double(elapsed);
    // The frame interval over the same frames the stage averages cover, so `other` is what they leave out.
    const double interval =
        done_at_.size() >= 2 ? double(done_at_.back() - done_at_.front()) / double(done_at_.size() - 1) : 0.0;
    int gaps = 0;
    for (const sv::media::MediaSource *v : waiting_.videos)
        gaps += v->stepping_gaps();
    if (encoder_.isEmpty() && active()) // shortcut: read from Qt's FFmpeg debug log; use a recorder API if one appears
        for (const QString &l : Session::log_tail(20, "Opening encoder"))
            encoder_ = l.mid(l.indexOf("Opening encoder") + 16).trimmed();
    return {
        {"active", active()},
        {"frame", frame_},
        {"frames", frames_},
        {"path", job_.path},
        {"error", error_},
        {"elapsedMs", elapsed},
        {"rate", rate},
        {"etaMs", rate > 0 ? (frames_ - frame_) * 1000.0 / rate : 0.0},
        {"queue", int(recorder_.queued())},
        {"videoGaps", gaps},
        {"uiMs", ui_ms_ ? ui_ms_() : 0.0},
        {"stages", QVariantMap{{"waitVideo", sum.wait_video / n},
                               {"waitDmx", sum.wait_dmx / n},
                               {"waitEncoder", sum.wait_encoder / n},
                               {"build", sum.build / n},
                               {"draw", sum.draw / n},
                               {"readback", sum.readback / n},
                               {"push", sum.push / n},
                               // the rest of the frame interval: UI work and timers between frames
                               {"other", std::max(0.0, interval - (sum.wait_video + sum.wait_dmx + sum.wait_encoder +
                                                                   sum.build + sum.draw + sum.readback + sum.push) /
                                                                      n)}}},
        {"width", job_.size.width()},
        {"height", job_.size.height()},
        {"fps", job_.fps},
        {"quality", job_.cinematic ? "cinematic" : "high"},
        {"samples", job_.samples},
        {"supersample", job_.supersample},
        {"backend", backend_},
        {"device", device_},
        {"encoder", encoder_},
        {"previewSerial", preview_serial_}};
}

void OfflineRender::next() {
    if (!active())
        return;
    if (frame_ >= frames_) {
        if (recorder_.queued() > 0)
            return QTimer::singleShot(50, this, &OfflineRender::next); // the encoder still owes frames
        return stop();
    }
    const double rel = frame_ * 1000.0 / job_.fps;
    t_ = job_.start_ms + rel;
    dmx_before_ = dmx_serial_ ? dmx_serial_() : 0;
    QElapsedTimer clock;
    clock.start();
    waiting_ = seek_ ? seek_(t_) : Inputs{};
    seek_ms_ = double(clock.elapsed());
    waited_.start();
    poll_.start();
    poll(); // inputs that are ready go ahead now, not after the first tick
}

void OfflineRender::poll() {
    const bool videos =
        std::ranges::all_of(waiting_.videos, [this](sv::media::MediaSource *v) { return v->shows(t_); });
    const bool dmx = !waiting_.dmx || !dmx_serial_ || dmx_serial_() != dmx_before_;
    const bool encoder = recorder_.queued() < 4;
    if (!encoder) {
        waited_on_ = &Stage::wait_encoder;
        return;
    }
    if (!(videos && dmx) && waited_.elapsed() < 3000) { // after 3 s the frame goes ahead regardless
        waited_on_ = videos ? &Stage::wait_dmx : &Stage::wait_video;
        return;
    }
    Stage st;
    if (waited_on_)
        st.*waited_on_ = double(waited_.elapsed());
    st.wait_video += seek_ms_;
    waited_on_ = nullptr;
    if (!(videos && dmx)) {
        const auto [from, to] = waiting_.videos.empty() ? std::pair{-1.0, -1.0} : waiting_.videos.front()->frame_span();
        qWarning("render: frame %d at %.1f ms timed out (video %.1f..%.1f, dmx %d)", frame_, t_, from, to, int(dmx));
    }
    poll_.stop();
    // Gathered here, on the UI thread, so the worker touches nothing the UI edits.
    // shortcut: the inputs stay at t across the shutter; seek per sub-frame if fast DMX moves look stepped.
    Session &s = *Session::instance();
    FrameInputs in;
    in.generation = generation_;
    in.project = s.snapshot(), in.library = s.library_snapshot(), in.meshes = s.meshes();
    if (s.revision() != revision_) {
        revision_ = s.revision();
        if (composition_.set_project(in.project, s.composition()))
            in.composition = composition_.cpu();
    }
    if (composition_.set_feeds(s.feed_sources()))
        in.composition = composition_.cpu();
    if (composition_.set_recorded(s.playback_feed_sources()))
        in.composition = composition_.cpu();
    composition_.set_opacity(s.playback_opacity());
    if (QImage c = composition_.update(s.media()); !c.isNull())
        in.composition = std::move(c);
    for (const auto &u : s.dmx().status())
        in.frames[u.universe] = u.data;
    for (int k = 0; k < job_.samples; ++k) {
        const double at = t_ + k * 500.0 / (job_.samples * job_.fps); // 180° shutter: the first half of the frame
        json camera = job_.view;
        camera.update(camera_at(job_.camera, at - job_.start_ms));
        in.subframes.emplace_back(camera_from(camera), at / 1000);
    }
    // Sub-frames are averaged and scaled down on the GPU; only the output frame is read back.
    QMetaObject::invokeMethod(
        &worker_context_,
        [this, in = std::move(in), st]() mutable {
            if (!view_) // stopped before this frame began
                return;
            bool failed = false;
            QImage image = view_->render(in, &failed, st);
            QMetaObject::invokeMethod(
                this,
                [this, generation = in.generation, image = std::move(image), st, failed] {
                    frame_done(generation, image, st, failed);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void OfflineRender::frame_done(quint64 generation, QImage image, Stage st, bool failed) {
    if (generation != generation_ || !active())
        return;
    if (failed)
        return stop(tr("The graphics device failed to draw a frame"));
    QElapsedTimer clock;
    clock.start();
    recorder_.push(image, qint64(std::llround(frame_ * 1e6 / job_.fps)));
    st.push = double(clock.elapsed());
    stages_.push_back(st);
    if (stages_.size() > 20)
        stages_.pop_front();
    const qint64 now = started_.elapsed();
    done_at_.push_back(now);
    if (done_at_.size() > 21) // one more than the stage ring: 20 intervals
        done_at_.pop_front();
    if (!preview_at_.isValid() || preview_at_.elapsed() > 1000) {
        preview_ = image.width() > 480 ? image.scaledToWidth(480, Qt::FastTransformation) : image;
        ++preview_serial_, preview_at_.start();
    }
    ++frame_;
    next();
}
