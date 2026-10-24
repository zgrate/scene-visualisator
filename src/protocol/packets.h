#pragma once
// Art-Net 4 and sACN (E1.31-2018) packet parsing and building. Parsers validate everything: input is untrusted.
#include "protocol/constants.h"

#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sv::proto {

using Bytes = std::vector<std::uint8_t>;
using Cid = std::array<std::uint8_t, 16>;

struct ArtDmx {
    std::uint16_t port_address;
    std::uint8_t sequence;
    std::span<const std::uint8_t> slots;
};
struct ArtSync {};
struct ArtPoll {};
struct ArtTimeCode {
    std::uint8_t frames, seconds, minutes, hours;
    std::uint8_t type; // 0 Film 24, 1 EBU 25, 2 DF 29.97, 3 SMPTE 30
    double fps() const { return type == 0 ? 24 : type == 1 ? 25 : type == 2 ? 29.97 : 30; }
    // Milliseconds since 00:00:00:00; frames count at the nominal rate (drop frame read as 30).
    std::int64_t ms() const {
        return ((hours * 60 + minutes) * 60 + seconds) * std::int64_t(1000) + frames * 1000 / (type == 0 ? 24 : type == 1 ? 25 : 30);
    }
};
using ArtNetPacket = std::variant<std::monostate, ArtDmx, ArtSync, ArtPoll, ArtTimeCode>;
ArtNetPacket parse_artnet(std::span<const std::uint8_t> packet);

struct SacnData {
    Cid cid;
    std::string source_name;
    std::uint8_t priority;
    std::uint16_t sync_address;
    std::uint8_t sequence;
    bool terminated;
    std::uint16_t universe;
    std::uint8_t start_code;
    std::span<const std::uint8_t> slots;
};
struct SacnSync {
    Cid cid;
    std::uint8_t sequence;
    std::uint16_t sync_address;
};
using SacnPacket = std::variant<std::monostate, SacnData, SacnSync>;
SacnPacket parse_sacn(std::span<const std::uint8_t> packet);

Bytes make_artdmx(std::uint16_t port_address, std::uint8_t sequence, std::span<const std::uint8_t> slots);
Bytes make_artsync();
Bytes make_artpoll();
Bytes make_arttimecode(const ArtTimeCode &tc);
// Up to 4 output ports; all must share Net and Sub-Net (the caller groups them). bind_index is 1-based.
Bytes make_artpollreply(std::array<std::uint8_t, 4> ip, std::string_view short_name, std::string_view long_name,
                        std::span<const std::uint16_t> port_addresses, std::uint8_t bind_index);
Bytes make_sacn_data(const Cid &cid, std::string_view source_name, std::uint8_t priority, std::uint8_t sequence,
                     std::uint16_t universe, std::span<const std::uint8_t> slots, std::uint8_t start_code = 0,
                     std::uint16_t sync_address = 0, bool terminated = false);
Bytes make_sacn_sync(const Cid &cid, std::uint8_t sequence, std::uint16_t sync_address);

} // namespace sv::proto
