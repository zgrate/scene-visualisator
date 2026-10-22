#include "protocol/constants.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Art-Net Port-Address packs Net:Sub-Net:Universe into 15 bits") {
    CHECK(sv::proto::port_address(0, 0, 1) == 1);
    CHECK(sv::proto::port_address(1, 2, 3) == 0x123);
    CHECK(sv::proto::port_address(0x7F, 0xF, 0xF) == 0x7FFF);
}

TEST_CASE("sACN multicast group is 239.255.U_hi.U_lo") {
    CHECK(sv::proto::sacn_multicast_group(1) == std::array<std::uint8_t, 4>{239, 255, 0, 1});
    CHECK(sv::proto::sacn_multicast_group(0x1234) == std::array<std::uint8_t, 4>{239, 255, 0x12, 0x34});
}
