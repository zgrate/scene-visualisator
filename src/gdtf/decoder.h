#pragma once
// FR-10: DMX -> physical attributes for one patched fixture. Runs on the render thread; no allocation per call
// except for pixel cells.
#include "gdtf/fixture_type.h"

namespace sv::gdtf {

struct Attributes {
    double dimmer = 0;                  // 0..1
    double pan = 0, tilt = 0;           // degrees
    double beam_angle = 0;              // degrees, from Zoom or the fixture's fixed beam
    double focus = 0, frost = 0;        // 0..1
    double iris = 1;                    // 0..1 open fraction
    Rgb colour{1, 1, 1};                // linear emitted colour (emitter x additive mix x wheel filter)
    int colour_slot = 0, gobo_slot = 0; // 1-based wheel slots, 0 = none/open
    double strobe_hz = 0;               // 0 = no strobe
    bool shutter_open = true;
    std::vector<Rgb> cells; // pixel cells (bars), in geometry order; empty for single-cell fixtures
};

// `universe` is the frame of the fixture's universe; `address` is 1-based. Slots past 512 read as 0.
Attributes decode(const FixtureType &type, const Mode &mode, std::span<const std::uint8_t> universe,
                  std::uint16_t address);

// Footprint offsets that merge HTP (intensity) under the priority policy.
std::vector<std::uint16_t> intensity_offsets(const Mode &mode);

} // namespace sv::gdtf
