#pragma once
// The network thread (spec "Threading"): Art-Net + sACN sockets, per-universe merge, internal sources, stats.
// Render thread reads frames through TripleBuffers; the UI reads status() snapshots. No Qt, no Asio in the header.
#include "protocol/generators.h"
#include "protocol/merger.h"
#include "protocol/triple_buffer.h"

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
