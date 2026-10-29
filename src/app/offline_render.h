#pragma once
// Offline render (section 26): steps show time one output frame at a time, so a slow GPU still writes a smooth video.
// For each frame it puts every input at time t (through `seek`), moves the camera along its keys, steps each video
// to the frame covering t, draws one frame at scene time t on its own offscreen QRhi, and encodes it stamped t. It
// never touches the window, so it keeps going while the window is minimised, covered or the screen is locked. Cinematic
// jobs light the rig with its own beams and walls, supersample 2× and blend sub-frames for motion blur. The frame
// loop, the inputs and the encoder live on the UI thread; the GPU work runs on a worker thread, so the window keeps
// repainting during a render.
#include "composition_builder.h"
#include "core/model.h"
#include "media/frame_recorder.h"

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QThread>
#include <QTimer>
#include <QVariantMap>

#include <deque>
#include <functional>
#include <memory>
#include <vector>

class QOffscreenSurface;

namespace sv::media {
class MediaSource;
}

// The camera at t (ms) along keys [{t, target, yaw, pitch, distance, fov}], eased between keys and held past both
// ends. A key may leave fields out: they hold the previous key's value. An empty list gives an empty object.
sv::json camera_at(const sv::json &keys, double t_ms);

class OfflineRender : public QObject {
    Q_OBJECT
  public:
    struct Job {
        QString path; // mp4
        double fps = 30, start_ms = 0, duration_ms = 0;
        QSize size{1920, 1080};
        bool lossless = false;
        sv::json camera = sv::json::array(); // keys for camera_at, t relative to start
        sv::json view = sv::json::object();  // cameraState of the 3D view: what the keys leave out
        bool cinematic = true;   // SceneRenderer::cinematic, drawn at 2× and scaled down; false: the viewport's look
        int samples = 0;         // sub-frames over a 180° shutter (motion blur); 0: 4 in cinematic, else 1
        bool supersample = true; // cinematic only: draw at 2× and scale down (~4× the GPU work)
    };
    struct Inputs {
        std::vector<sv::media::MediaSource *> videos; // wait until each shows t
        bool dmx = false;                             // wait for the DMX engine to publish
    };
    // seek(t_ms) puts the inputs at t; dmx_serial reads DmxEngine::publish_serial; done runs after the file closes.
    // ui_ms reports the UI's status-tick cost (Session), shown beside the `other` stage.
    QString start(Job job, std::function<Inputs(double)> seek, std::function<quint64()> dmx_serial,
                  std::function<void()> done, std::function<double()> ui_ms = {});
    void stop(const QString &error = {}); // waits for the frame in flight
    OfflineRender();
    ~OfflineRender() override;
    bool active() const { return recorder_.active(); }
    // {active, frame, frames, path, error, elapsedMs, rate (frames/s over the last 2 s), etaMs, queue, stages
    // {waitVideo, waitDmx, waitEncoder, build, draw, readback, push} (ms per frame, last 20 frames), width, height,
    // fps, quality, samples, backend, device, encoder, previewSerial}
    QVariantMap status() const;
    const QImage &preview() const { return preview_; } // the last output frame at 480 px wide, every 0.5 s
    quint64 preview_serial() const { return preview_serial_; }

    // Where one output frame's time went, ms. wait_*: what poll() waited for before drawing.
    struct Stage {
        double wait_video = 0, wait_dmx = 0, wait_encoder = 0, build = 0, draw = 0, readback = 0, push = 0;
    };

  private:
    void next();
    void poll();
    void frame_done(quint64 generation, QImage image, Stage st, bool failed); // UI thread, from the worker
    void shutdown_view();                                                     // destroys the View on the worker

    struct View; // the offscreen QRhi, its target and the scene renderer: made, used and destroyed on worker_
    std::unique_ptr<View> view_;
    QThread worker_;
    QObject worker_context_;                     // lives on worker_: the target of the invokes
    quint64 generation_ = 0;                     // stop() bumps it, so a frame finishing late is dropped
    std::unique_ptr<QOffscreenSurface> surface_; // OpenGL only; a GUI-thread object that outlives the QRhi
    CompositionBuilder composition_;             // UI thread: the walls' image, handed to the worker when it changes
    quint64 revision_ = ~0ull;
    Job job_;
    std::function<Inputs(double)> seek_;
    std::function<quint64()> dmx_serial_;
    std::function<double()> ui_ms_;
    std::function<void()> done_;
    sv::media::FrameRecorder recorder_;
    QTimer poll_;
    QElapsedTimer waited_;
    Inputs waiting_;
    quint64 dmx_before_ = 0;
    int frame_ = 0, frames_ = 0;
    double t_ = 0;
    QString error_;
    QElapsedTimer started_, preview_at_;
    std::deque<Stage> stages_;           // the last 20 frames
    std::deque<qint64> done_at_;         // ms since start of the last 21 finished frames
    double Stage::*waited_on_ = nullptr; // the bucket poll() last waited for
    double seek_ms_ = 0;                 // the seek call itself, booked to wait_video
    QString backend_, device_;
    QImage preview_;
    quint64 preview_serial_ = 0;
    mutable QString encoder_; // from Qt's FFmpeg log, once seen
};
