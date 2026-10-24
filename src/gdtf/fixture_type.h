#pragma once
// The alpha subset of GDTF 1.2 (github.com/mvrdevelopment/spec): fixture type, DMX modes, channels, channel
// functions with physical ranges, wheels, the first beam and the body size. Unknown elements are ignored.
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sv::gdtf {

struct GdtfError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

using Rgb = std::array<float, 3>; // linear, 0..1

struct ChannelSet {
    std::uint32_t dmx_from; // at the channel's resolution
    int wheel_slot;         // 1-based, 0 = none
};

struct ChannelFunction {
    std::string name;
    std::string attribute;      // GDTF attribute name, e.g. "Dimmer", "Pan", "ColorAdd_R", "Color1", "Gobo1"
    std::uint32_t dmx_from = 0; // at the channel's resolution
    double physical_from = 0, physical_to = 1;
    std::string wheel; // wheel name, empty if none
    std::vector<ChannelSet> sets;
};

struct Channel {
    std::vector<std::uint16_t> offsets; // 0-based slots from the fixture address, coarse first
    std::string geometry;
    std::uint32_t default_value = 0;
    std::vector<ChannelFunction> functions; // sorted by dmx_from
    int head = -1;                          // index into Mode::heads, -1 = the whole fixture
    const std::string &attribute() const { return functions.front().attribute; }
};

// One moving head of a multi-head fixture: the outermost Axis geometry that carries a Pan or Tilt channel.
struct Head {
    std::array<double, 3> offset{0, 0, 0}; // pivot in body axes (Y up), metres
};

struct Mode {
    std::string name;
    std::vector<Channel> channels;
    std::uint16_t footprint = 0;
    std::vector<Head> heads;
};

struct WheelSlot {
    std::string name;
    Rgb colour{1, 1, 1};
};
struct Wheel {
    std::string name;
    std::vector<WheelSlot> slots;
};

struct FixtureType {
    std::string name, short_name, manufacturer, id;
    std::vector<Mode> modes;
    std::vector<Wheel> wheels;
    Rgb emitter{1, 1, 1}; // first emitter colour; white if the file has none
    double beam_angle = 25, field_angle = 30, luminous_flux = 10000, beam_radius = 0.05; // degrees, lm, m
    std::array<double, 3> body{0.3, 0.4, 0.3};      // length, height, width of the first model, metres
    std::array<double, 3> beam_direction{0, -1, 0}; // first beam at rest, in body axes (Y up), from its geometry chain
    bool has_beam = true; // false when the file has geometry but no Beam (hazers, risers): it never emits light

    const Mode *mode(std::string_view name) const;
    const Wheel *wheel(std::string_view name) const;
};

FixtureType parse_description(std::string_view xml); // throws GdtfError
FixtureType load_gdtf(std::span<const std::uint8_t> zip);
FixtureType load_gdtf(const std::filesystem::path &path);

Rgb cie_to_rgb(double x, double y);               // CIE 1931 xy chromaticity -> linear sRGB, max channel scaled to 1
std::array<double, 2> rgb_to_cie(const Rgb &rgb); // linear sRGB -> CIE xy, the inverse up to brightness

// FR-4: a GDTF 1.2 description.xml that parse_description reads back as the same type. Geometry is fixed
// (Base > Yoke > Head > Beam, Pan on Yoke, Tilt on Head, the rest on Beam); one Model from body.
// shortcut: channel geometries, GeometryReferences and 3D models are not written; add when the builder edits them.
std::string write_description(const FixtureType &type);

} // namespace sv::gdtf
