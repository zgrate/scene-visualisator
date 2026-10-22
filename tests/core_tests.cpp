#include "core/units.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("degrees and radians round trip") {
    CHECK(sv::to_degrees(sv::to_radians(270.0)) == Catch::Approx(270.0));
    CHECK(sv::to_radians(180.0) == Catch::Approx(3.14159265358979));
}
