#pragma once
// FR-13 NDI input, and NDI output of the Resolume feeds (signal output). The runtime is loaded at run time (never linked or shipped): from $NDI_RUNTIME_DIR_V6, then the
// system library path. Without it, NDI is simply unavailable.
#include <cstddef> // the NDI headers use NULL without including it

#include <Processing.NDI.Lib.h>

#include <QImage>
#include <QStringList>

#include <atomic>
#include <functional>
#include <thread>

namespace sv::media {

class MediaSource;

// The loaded runtime, initialised on first call; null when absent. Thread-safe.
const NDIlib_v6 *ndi_api();

// NFR-9: discovery runs only when asked. Blocks up to `wait_ms` collecting source names ("HOST (Name)").
QStringList ndi_sources(int wait_ms = 1000);

// Receives one source by name on its own thread at highest bandwidth, BGRA, audio dropped. The runtime reconnects by
// name on its own when the sender goes and comes back. Frames are deep-copied and handed to `on_frame`.
class NdiReceiver {
  public:
    NdiReceiver(const QString &source, std::function<void(QImage)> on_frame);
    ~NdiReceiver();
    NdiReceiver(const NdiReceiver &) = delete;
    NdiReceiver &operator=(const NdiReceiver &) = delete;

  private:
    std::atomic_bool stop_{false};
    std::thread thread_;
};

// Sends whatever `set_source` points at as one NDI stream, from its own thread polling the source's latest-wins slot.
// Without a runtime it does nothing.
class NdiSender {
  public:
    explicit NdiSender(const QString &name);
    ~NdiSender();
    NdiSender(const NdiSender &) = delete;
    NdiSender &operator=(const NdiSender &) = delete;
    // Sources are pooled and outlive the sender; null pauses the stream.
    void set_source(const MediaSource *source) { source_ = source; }
    double fps() const { return fps_; }

  private:
    std::atomic<const MediaSource *> source_{nullptr};
    std::atomic<double> fps_{0};
    std::atomic_bool stop_{false};
    std::thread thread_;
};

} // namespace sv::media
