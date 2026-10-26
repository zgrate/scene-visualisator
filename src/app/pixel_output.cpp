#include "pixel_output.h"

#include "render/test_pattern.h"

#include <algorithm>
#include <chrono>

void PixelOutput::set(Inputs inputs) {
    const bool active = !inputs.targets.empty();
    {
        std::lock_guard lock(mutex_);
        pending_ = std::move(inputs), dirty_ = true, stop_ = !active;
    }
    wake_.notify_all();
    if (!active && thread_.joinable())
        thread_.join();
    else if (active && !thread_.joinable())
        thread_ = std::thread([this] { run(); });
}

void PixelOutput::run() {
    CompositionBuilder builder;
    std::vector<Target> targets;
    std::shared_ptr<const sv::json> project;
    double rate_hz = 44;
    QImage current;
    std::unique_lock lock(mutex_);
    while (!stop_) {
        bool resample = false;
        if (std::exchange(dirty_, false)) {
            project = pending_.project;
            if (builder.set_project(pending_.project, pending_.cpu))
                current = builder.cpu(), resample = true;
            if (builder.set_feeds(pending_.feeds))
                current = builder.cpu(), resample = true;
            if (builder.set_recorded(pending_.recorded))
                current = builder.cpu(), resample = true;
            builder.set_opacity(pending_.opacity);
            resample = resample || targets != pending_.targets;
            targets = pending_.targets, rate_hz = std::clamp(pending_.rate_hz, 1.0, 100.0);
        }
        lock.unlock();
        if (QImage c = builder.update(media_); !c.isNull())
            current = std::move(c), resample = true;
        if (resample && project) // the engine resends the last pixels at its own rate
            for (const Target &t : targets)
                dmx_.set_ddp_pixels(
                    t.key, sv::render::sample_slice(current, *project, t.slice, t.cols, t.rows, t.column_major));
        lock.lock();
        wake_.wait_for(lock, std::chrono::microseconds(std::int64_t(1e6 / rate_hz)),
                       [this] { return dirty_ || stop_; });
    }
}
