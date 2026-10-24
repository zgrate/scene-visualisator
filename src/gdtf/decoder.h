#pragma once
// FR-10: DMX -> physical attributes for one patched fixture. Runs on the render thread; no allocation per call
// except for pixel cells.
#include "gdtf/fixture_type.h"

namespace sv::gdtf {

// One head of a multi-head fixture (Mode::heads); the dimmer, shutter and strobe stay fixture-wide.
struct HeadState {
    double pan = 0, tilt = 0, beam_angle = 0; // degrees
    Rgb colour{1, 1, 1};                      // mean of the head's cells x wheel filter
};

struct Attributes {
    double dimmer = 0;                  // 0..1
    double pan = 0, tilt = 0;           // degrees
    double beam_angle = 0;              // degrees, from Zoom or the fixture's fixed beam
    double focus = 0, frost = 0;        // 0..1
    double iris = 1;                    // 0..1 open fraction
    Rgb colour{1, 1, 1};                // linear emitted colour (emitter x additive mix x wheel filter)
    int colour_slot = 0, gobo_slot = 0; // 1-based wheel slots, 0 = none/open
    double strobe_hz = 0;               // 0 = no strobe
    double haze = 0;                    // 0..1 output of a fog machine or hazer (Fog*, Haze* attributes)
    bool shutter_open = true;
    std::vector<Rgb> cells;       // pixel cells (bars), in geometry order; empty for single-cell fixtures
    std::vector<HeadState> heads; // one per Mode::heads when there are two or more; empty otherwise
};

// `universe` is the frame of the fixture's universe; `address` is 1-based. Slots past 512 read as 0.
Attributes decode(const FixtureType &type, const Mode &mode, std::span<const std::uint8_t> universe,
                  std::uint16_t address);

// One DMX byte at an offset from the fixture address.
struct DmxByte {
    std::uint16_t offset;
    std::uint8_t value;
};
// decode() in reverse for one attribute: the bytes that make every channel function carrying it (on head `head`, or
// on every head when -1) decode to `value` in physical units, or to wheel slot `value` when the function has
// ChannelSets. Empty when the mode has no such function.
std::vector<DmxByte> encode(const Mode &mode, std::string_view attribute, double value, int head = -1);
// The bytes that open every Shutter channel: its first function named "Open", else its first that is neither
// "Closed" nor a strobe.
std::vector<DmxByte> open_shutter(const Mode &mode);

// Footprint offsets that merge HTP (intensity) under the priority policy.
std::vector<std::uint16_t> intensity_offsets(const Mode &mode);

// True when values of the attribute in between two others make sense (Dimmer, Pan, colour mix, Zoom...), so it can
// crossfade; wheels, shutter/strobe and control channels must switch instead.
bool continuous_attribute(std::string_view attribute);

} // namespace sv::gdtf
