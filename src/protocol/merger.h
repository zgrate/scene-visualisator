#pragma once
// Per-universe source merge (spec "DMX merge"): highest priority wins; on a tie intensity slots merge HTP and all
// other slots LTP. The htp/ltp policies ignore priority.
#include "protocol/constants.h"

#include <bitset>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sv::proto {

enum class MergePolicy { priority, htp, ltp };
using Clock = std::chrono::steady_clock;

struct Source {
    std::string id;   // stable key, e.g. "artnet:10.0.0.5" or "sacn:<cid>"
    std::string name; // shown in the UI
    Slots data{};
    std::uint16_t count = 0;
    std::uint8_t priority = 100;
    std::optional<Slots> slot_priority; // sACN 0xDD; 0 = this source does not drive the slot
    Clock::time_point last_seen{};
    Clock::duration timeout = Clock::duration::max(); // internal sources never time out
    std::uint64_t serial = 0;                         // receive order, for LTP
};

class UniverseMerger {
  public:
    static constexpr std::uint8_t no_winner = 0xFF;
    struct Result {
        Slots data{};
        std::array<std::uint8_t, 512> winner; // index into sources(), or no_winner
    };

    explicit UniverseMerger(MergePolicy policy = MergePolicy::priority) : policy_(policy) {}
    void set_policy(MergePolicy p) { policy_ = p; }
    MergePolicy policy() const { return policy_; }
    void set_intensity_mask(const std::bitset<512> &m) { intensity_ = m; }

    void receive(const std::string &id, const std::string &name, std::span<const std::uint8_t> slots,
                 std::uint8_t priority, Clock::time_point now, Clock::duration timeout);
    void receive_slot_priority(const std::string &id, std::span<const std::uint8_t> priorities, Clock::time_point now);
    bool expire(Clock::time_point now); // true if a source was dropped
    bool remove(const std::string &id);
    Result merge() const;
    const std::vector<Source> &sources() const { return sources_; }

  private:
    Source &find_or_add(const std::string &id);
    MergePolicy policy_;
    std::bitset<512> intensity_;
    std::vector<Source> sources_;
    std::uint64_t serial_ = 0;
};

// Show playback mixed into the live merge per slot (live -> recording by `weight`).
struct PlaybackMix {
    Slots frame{};                          // the recording's state
    std::array<std::uint8_t, 512> weight{}; // 0 = live, 255 = recording, between = crossfade
    std::bitset<512> snap;                  // switches at 128 instead of fading: wheels, control, unpatched slots
    std::bitset<512> htp;                   // max(live, recording) instead of the weight
    std::bitset<512> fine;                  // the fine byte of the slot before it: faded as one 16-bit value
};
// The mixed slots; `from_recording` (optional) marks the slots the recording decides.
Slots mix_playback(const Slots &live, const PlaybackMix &mix, std::bitset<512> *from_recording = nullptr);

} // namespace sv::proto
