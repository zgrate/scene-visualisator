#include "protocol/merger.h"

#include <algorithm>

namespace sv::proto {

Source &UniverseMerger::find_or_add(const std::string &id) {
    auto it = std::ranges::find(sources_, id, &Source::id);
    if (it != sources_.end())
        return *it;
    // shortcut: more than 254 sources on one universe are ignored by winner tracking, fine for real rigs.
    return sources_.emplace_back(Source{.id = id});
}

void UniverseMerger::receive(const std::string &id, const std::string &name, std::span<const std::uint8_t> slots,
                             std::uint8_t priority, Clock::time_point now, Clock::duration timeout) {
    Source &s = find_or_add(id);
    s.name = name;
    s.count = std::uint16_t(std::min<std::size_t>(slots.size(), 512));
    std::copy_n(slots.begin(), s.count, s.data.begin());
    std::fill(s.data.begin() + s.count, s.data.end(), 0);
    s.priority = priority;
    s.last_seen = now;
    s.timeout = timeout;
    s.serial = ++serial_;
}

void UniverseMerger::receive_slot_priority(const std::string &id, std::span<const std::uint8_t> priorities,
                                           Clock::time_point now) {
    Source &s = find_or_add(id);
    Slots p{};
    std::copy_n(priorities.begin(), std::min<std::size_t>(priorities.size(), 512), p.begin());
    s.slot_priority = p;
    s.last_seen = now;
}

bool UniverseMerger::expire(Clock::time_point now) {
    return std::erase_if(sources_, [&](const Source &s) {
               return s.timeout != Clock::duration::max() && now - s.last_seen > s.timeout;
           }) > 0;
}

bool UniverseMerger::remove(const std::string &id) {
    return std::erase_if(sources_, [&](auto &s) { return s.id == id; }) > 0;
}

UniverseMerger::Result UniverseMerger::merge() const {
    Result r;
    r.winner.fill(no_winner);
    std::array<int, 512> best_priority;
    best_priority.fill(-1);
    for (std::size_t i = 0; i < sources_.size() && i < no_winner; ++i) {
        const Source &s = sources_[i];
        for (std::size_t slot = 0; slot < s.count; ++slot) {
            int prio = s.slot_priority ? (*s.slot_priority)[slot] : s.priority;
            if (prio == 0 && s.slot_priority)
                continue; // per-address priority 0: no contribution
            if (policy_ != MergePolicy::priority)
                prio = 0;
            const std::uint8_t w = r.winner[slot];
            bool take;
            if (w == no_winner || prio > best_priority[slot])
                take = true;
            else if (prio < best_priority[slot])
                take = false;
            else if (policy_ == MergePolicy::htp || (policy_ == MergePolicy::priority && intensity_[slot]))
                take = s.data[slot] > r.data[slot];
            else // shortcut: LTP picks the most recently received source, not the most recently changed slot
                take = s.serial > sources_[w].serial;
            if (take) {
                r.data[slot] = s.data[slot];
                r.winner[slot] = std::uint8_t(i);
                best_priority[slot] = prio;
            }
        }
    }
    return r;
}

Slots mix_playback(const Slots &live, const PlaybackMix &mix, std::bitset<512> *from_recording) {
    Slots out = live;
    std::bitset<512> rec;
    for (std::size_t i = 0; i < 512; ++i) {
        const int w = mix.weight[i];
        if (w == 0)
            continue;
        const bool wide = i + 1 < 512 && mix.fine[i + 1]; // a 16-bit pair, coarse first
        const auto value = [&](const Slots &s) { return wide ? s[i] << 8 | s[i + 1] : int(s[i]); };
        const int l = value(live), r = value(mix.frame);
        int v;
        if (mix.htp[i])
            v = std::max(l, r), rec[i] = r > l;
        else if (mix.snap[i] || w == 255)
            v = w >= 128 ? r : l, rec[i] = w >= 128;
        else
            v = (l * (255 - w) + r * w + 127) / 255, rec[i] = w >= 128;
        if (wide) {
            out[i] = std::uint8_t(v >> 8), out[i + 1] = std::uint8_t(v), rec[i + 1] = rec[i];
            ++i;
        } else {
            out[i] = std::uint8_t(v);
        }
    }
    if (from_recording)
        *from_recording = rec;
    return out;
}

} // namespace sv::proto
