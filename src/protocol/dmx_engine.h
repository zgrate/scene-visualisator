#pragma once
// The network thread (spec "Threading"): Art-Net + sACN sockets, per-universe merge, internal sources, stats.
// Render thread reads frames through TripleBuffers; the UI reads status() snapshots. No Qt, no Asio in the header.
#include "protocol/generators.h"
#include "protocol/merger.h"
#include "protocol/triple_buffer.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace sv::proto {

struct EngineConfig {
    bool artnet = true, sacn = true;
    std::uint16_t artnet_port = proto::artnet_port; // 0 = ephemeral (tests)
    std::uint16_t sacn_port = proto::sacn_port;
    std::string bind_address = "0.0.0.0";
    std::string sacn_interface = "0.0.0.0"; // IPv4 address of the NIC that joins multicast groups
    std::string node_name = "Scene Visualiser";
};

// Signal output (proxy): what the engine publishes goes out again, at `universe + universe_offset`.
struct OutputConfig {
    bool enabled = false;
    bool sacn = false;          // false = Art-Net
    std::string target;         // unicast IPv4; empty = Art-Net broadcast 255.255.255.255 or sACN multicast
    std::uint16_t port = 0;     // 0 = the protocol's port (tests use their own)
    int universe_offset = 0;
    std::uint8_t priority = 100; // sACN
    double rate_hz = 44;         // changed universes per second; unchanged ones are refreshed once a second
    bool operator==(const OutputConfig &) const = default;
};

struct OutputStatus {
    bool enabled = false;
    double packets_per_s = 0;
    std::size_t universes = 0; // sent in the last second
    std::string error;
    std::size_t ddp_targets = 0;
    double ddp_packets_per_s = 0;
    std::string ddp_error;
};

// DDP pixel output (WLED, ESPixelStick, Falcon): one object's pixels as one byte stream at `offset` into a
// controller's buffer, sent at the output rate whether or not universe output is on.
struct DdpTarget {
    std::string key; // object uuid
    std::string ip;  // IPv4
    std::uint16_t port = ddp_port;
    std::uint32_t offset = 0; // bytes
    bool rgbw = false;
    // Bytes taken from the published universes as (universe, 1-based slot); slot 0 sends 0. Empty: the bytes come
    // from set_ddp_pixels (composition-fed walls and strips).
    std::vector<std::pair<std::uint16_t, std::uint16_t>> channels;
    bool operator==(const DdpTarget &) const = default;
};

struct UniverseFrame {
    Slots data{};
    std::uint64_t serial = 0;
};

struct UniverseStatus {
    std::uint16_t universe = 0;
    Slots data{};
    double rate_hz = 0;        // merged frames per second over the last second
    std::uint64_t lost = 0;    // sequence gaps since subscribe
    std::uint64_t dropped = 0; // out-of-order packets discarded
    std::vector<std::string> sources;    // display names
    std::vector<std::string> source_ids; // same order: "artnet:<ip>", "sacn:<cid>", "internal:..."
    std::array<std::uint8_t, 512> winner{}; // index into sources, UniverseMerger::no_winner if none
    MergePolicy policy = MergePolicy::priority;
};

// The last ArtTimeCode received; ms is -1 when none arrived in the last second.
struct Timecode {
    std::int64_t ms = -1;
    double fps = 25;
};

class DmxEngine {
  public:
    explicit DmxEngine(EngineConfig config = {});
    ~DmxEngine();
    DmxEngine(const DmxEngine &) = delete;
    DmxEngine &operator=(const DmxEngine &) = delete;

    void start(); // throws std::system_error if a socket cannot bind
    void stop();
    std::uint16_t artnet_bound_port() const { return artnet_bound_; }
    std::uint16_t sacn_bound_port() const { return sacn_bound_; }

    // Thread-safe setters; applied on the network thread.
    void subscribe(std::vector<std::uint16_t> universes); // at most max_universes, 1-based
    void set_policy(std::uint16_t universe, MergePolicy policy);
    void set_intensity_mask(std::uint16_t universe, std::bitset<512> mask);
    void set_internal_frame(std::uint16_t universe, std::optional<Slots> frame); // paste-a-frame / fader page
    void set_generator(Pattern pattern, double speed, std::vector<GeneratorTarget> targets);
    // Show playback, mixed per slot with the live merge (see PlaybackMix). nullopt clears it.
    void set_playback(std::uint16_t universe, std::optional<PlaybackMix> mix);
    // Light control: slots set in the mask override whatever the merge or playback gives. nullopt clears it.
    void set_programmer(std::uint16_t universe, std::optional<std::pair<Slots, std::bitset<512>>> programmer);
    // Called on the network thread with every published universe (show recording). Empty clears it.
    void set_tap(std::function<void(std::uint16_t, const Slots &)> tap);

    // Sends every published universe out; packets from our own output socket are never ingested.
    void set_output(OutputConfig config);
    OutputStatus output_status() const;
    void set_ddp(std::vector<DdpTarget> targets);
    void set_ddp_pixels(const std::string &key, std::vector<std::uint8_t> pixels); // any thread, latest wins

    Timecode timecode() const; // any thread
    std::uint64_t publish_serial() const; // any thread; advances with every universe the merge publishes

    // Render thread: one consumer per universe; the pointer stays valid for the engine's lifetime.
    TripleBuffer<UniverseFrame> *frames(std::uint16_t universe);
    // UI thread: copy of the latest state of every subscribed universe.
    std::vector<UniverseStatus> status() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::uint16_t artnet_bound_ = 0, sacn_bound_ = 0;
};

} // namespace sv::proto
