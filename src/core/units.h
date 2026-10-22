#pragma once
// Internal units: metres, degrees, Y-up, Z toward the audience. Convert only at file boundaries.
#include <numbers>

namespace sv {

constexpr double to_radians(double degrees) { return degrees * std::numbers::pi / 180.0; }
constexpr double to_degrees(double radians) { return radians * 180.0 / std::numbers::pi; }

} // namespace sv
