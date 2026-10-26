#include "composition_builder.h"

#include "render/test_pattern.h"

#include <algorithm>

void CompositionBuilder::reset() {
    base_ = {};
    media_serial_ = 0; // a live source's current frame goes back over the redrawn composition
    for (auto &[uuid, feed] : feeds_)
        feed.serial = 0;
    for (auto &[uuid, feed] : recorded_)
        feed.serial = 0;
}

bool CompositionBuilder::set_project(std::shared_ptr<const sv::json> project, QImage cpu) {
    if (project == project_ && cpu.cacheKey() == cpu_.cacheKey())
        return false;
    project_ = std::move(project), cpu_ = std::move(cpu);
    reset();
    return true;
}

bool CompositionBuilder::set(std::map<std::string, Feed> &feeds, const Sources &sources) {
    if (sources.size() == feeds.size() && std::ranges::equal(sources, feeds, [](const auto &a, const auto &b) {
            return a.first == b.first && a.second == b.second.source;
        }))
        return false;
    feeds.clear();
    for (const auto &[uuid, source] : sources)
        feeds[uuid] = {source};
    reset();
    return true;
}

bool CompositionBuilder::set_feeds(const Sources &live) { return set(feeds_, live); }
bool CompositionBuilder::set_recorded(const Sources &recorded) { return set(recorded_, recorded); }

void CompositionBuilder::set_opacity(std::map<std::string, double> opacity) {
    if (opacity != opacity_)
        opacity_ = std::move(opacity), opacity_changed_ = true;
}

QImage CompositionBuilder::update(const sv::media::MediaSource &media) {
    bool fresh = false;
    if (QImage frame = media.latest(media_serial_); !frame.isNull()) {
        // Placed by source.fit unless it already matches the composition. shortcut: a CPU blit per frame;
        // scale in the wall shader when 4K sources make it show up in profiles (NFR-2).
        base_ =
            project_ && frame.size() != cpu_.size() ? sv::render::make_composition(*project_, frame) : std::move(frame);
        fresh = true;
    }
    const auto poll = [&](std::map<std::string, Feed> &feeds) {
        std::map<std::string, QImage> frames;
        for (auto &[uuid, feed] : feeds) {
            if (QImage frame = feed.source->latest(feed.serial); !frame.isNull())
                feed.frame = std::move(frame), fresh = true;
            if (!feed.frame.isNull())
                frames[uuid] = feed.frame;
        }
        return frames;
    };
    const auto frames = poll(feeds_), recorded_frames = poll(recorded_);
    fresh = std::exchange(opacity_changed_, false) || fresh;
    if (!fresh || !project_)
        return {};
    return sv::render::compose_feeds(*project_,
                                     sv::render::compose_feeds(*project_, base_.isNull() ? cpu_ : base_, frames),
                                     recorded_frames, opacity_);
}
