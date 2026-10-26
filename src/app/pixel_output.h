#pragma once
// DDP pixel output for composition-fed walls and strips. Its own thread composes what the walls show (the viewport
// is paused while a screen covers it, and LEDs must keep running) and hands each target's sampled pixels to the DMX
// engine, which sends them. DMX-fed strips need none of this: the engine reads their bytes from the universes.
#include "composition_builder.h"
#include "protocol/dmx_engine.h"

#include <condition_variable>
#include <mutex>
#include <thread>

class PixelOutput {
  public:
    struct Target {
        std::string key; // object uuid, as in its DdpTarget
        sv::json slice;
        int cols = 1, rows = 1;
        bool column_major = false; // LED-string walls run column by column
        bool operator==(const Target &) const = default;
    };
    struct Inputs {
        std::shared_ptr<const sv::json> project;
        QImage cpu;
        CompositionBuilder::Sources feeds, recorded;
        std::map<std::string, double> opacity;
        std::vector<Target> targets;
        double rate_hz = 44;
    };

    PixelOutput(sv::proto::DmxEngine &dmx, const sv::media::MediaSource &media) : dmx_(dmx), media_(media) {}
    ~PixelOutput() { set({}); }

    // UI thread. Starts the thread when there are targets, stops it when there are none.
    void set(Inputs inputs);

  private:
    void run();

    sv::proto::DmxEngine &dmx_;
    const sv::media::MediaSource &media_;
    std::mutex mutex_;
    std::condition_variable wake_;
    Inputs pending_;
    bool dirty_ = false, stop_ = false;
    std::thread thread_;
};
