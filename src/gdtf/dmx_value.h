#pragma once
// Combine 1..4 DMX slots (coarse first, as GDTF Offset lists them) into one value.
#include <cstdint>
#include <span>

namespace sv::gdtf {

constexpr std::uint32_t dmx_value(std::span<const std::uint8_t> frame, std::size_t first, std::size_t bytes) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < bytes; ++i)
        v = (v << 8) | (first + i < frame.size() ? frame[first + i] : 0u);
    return v;
}

// Value scaled to 0..1 for a given byte count.
constexpr double dmx_normalised(std::uint32_t value, std::size_t bytes) {
    return double(value) / double((std::uint64_t{1} << (8 * bytes)) - 1);
}

} // namespace sv::gdtf
