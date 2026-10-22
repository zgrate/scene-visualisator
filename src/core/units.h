#pragma once
// Internal units: metres, degrees, Y-up, Z toward the audience. Convert only at file and UI boundaries.
#include <numbers>

namespace sv {

constexpr double to_radians(double degrees) { return degrees * std::numbers::pi / 180.0; }
constexpr double to_degrees(double radians) { return radians * 180.0 / std::numbers::pi; }

// FR-2: the UI may show feet; the document always stores metres.
constexpr double metres_per_foot = 0.3048;
constexpr double feet_to_metres(double feet) { return feet * metres_per_foot; }
constexpr double metres_to_feet(double metres) { return metres / metres_per_foot; }

} // namespace sv
