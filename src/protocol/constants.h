#pragma once
// Protocol constants from the spec "Protocol specification" section.
#include <array>
#include <chrono>
#include <cstdint>

namespace sv::proto {

inline constexpr std::uint16_t artnet_port = 6454;
inline constexpr std::uint16_t sacn_port = 5568;
inline constexpr auto artnet_timeout = std::chrono::milliseconds(3000);
inline constexpr auto sacn_timeout = std::chrono::milliseconds(2500);

// Art-Net 4 15-bit Port-Address: Net (7 bits) : Sub-Net (4 bits) : Universe (4 bits).
constexpr std::uint16_t port_address(unsigned net, unsigned subnet, unsigned universe) {
    return std::uint16_t(((net & 0x7Fu) << 8) | ((subnet & 0xFu) << 4) | (universe & 0xFu));
}

// sACN multicast group 239.255.U_hi.U_lo.
constexpr std::array<std::uint8_t, 4> sacn_multicast_group(std::uint16_t universe) {
    return {239, 255, std::uint8_t(universe >> 8), std::uint8_t(universe & 0xFF)};
}

} // namespace sv::proto
