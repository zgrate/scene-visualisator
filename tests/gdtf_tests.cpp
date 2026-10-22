#include "gdtf/dmx_value.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("coarse/fine DMX slots combine big-endian") {
    const std::array<std::uint8_t, 4> frame{0x80, 0x00, 0xFF, 0xFF};
    CHECK(sv::gdtf::dmx_value(frame, 0, 1) == 0x80);
    CHECK(sv::gdtf::dmx_value(frame, 0, 2) == 0x8000);
    CHECK(sv::gdtf::dmx_normalised(sv::gdtf::dmx_value(frame, 2, 2), 2) == 1.0);
    CHECK(sv::gdtf::dmx_value(frame, 3, 2) == 0xFF00); // past the frame end reads as 0
}
