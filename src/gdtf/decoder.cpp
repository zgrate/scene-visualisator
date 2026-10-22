#include "gdtf/decoder.h"

#include "gdtf/dmx_value.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace sv::gdtf {

namespace {

struct Decoded {
    const ChannelFunction *fn;
    double physical;
    int slot; // wheel slot, 1-based, 0 = none
};

Decoded decode_channel(const Channel &ch, std::span<const std::uint8_t> frame, std::size_t base) {
    const std::size_t bytes = ch.offsets.size();
    std::uint32_t v = 0;
    for (auto o : ch.offsets)
        v = (v << 8) | (base + o < frame.size() ? frame[base + o] : 0u);
    const std::uint32_t max = std::uint32_t((std::uint64_t{1} << (8 * bytes)) - 1);
    auto it = std::ranges::upper_bound(ch.functions, v, {}, &ChannelFunction::dmx_from);
    const ChannelFunction &fn = it == ch.functions.begin() ? ch.functions.front() : *std::prev(it);
    const std::uint32_t to = it == ch.functions.end() ? max : it->dmx_from - 1;
    const double span = to > fn.dmx_from ? double(to - fn.dmx_from) : 1.0;
    const double k = std::clamp(double(v - std::min(v, fn.dmx_from)) / span, 0.0, 1.0);
    int slot = 0;
    for (const ChannelSet &s : fn.sets)
        if (s.dmx_from <= v && s.wheel_slot > 0)
            slot = s.wheel_slot;
    return {&fn, fn.physical_from + k * (fn.physical_to - fn.physical_from), slot};
}

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

} // namespace

Attributes decode(const FixtureType &type, const Mode &mode, std::span<const std::uint8_t> universe,
                  std::uint16_t address) {
    Attributes a;
    a.beam_angle = type.beam_angle;
    const std::size_t base = address >= 1 ? address - 1 : 0;
    Rgb add{0, 0, 0}, filter{1, 1, 1};
    bool has_add = false, has_dimmer = false;
    std::vector<std::string_view> cell_geometry;
    auto cell = [&](const std::string &geometry) -> Rgb & {
        auto it = std::ranges::find(cell_geometry, geometry);
        if (it == cell_geometry.end()) {
            cell_geometry.push_back(geometry);
            a.cells.push_back({0, 0, 0});
            return a.cells.back();
        }
        return a.cells[std::size_t(it - cell_geometry.begin())];
    };
    // A fixture is multi-cell when its additive colour channels sit on more than one geometry.
    std::vector<std::string_view> colour_geometries;
    for (const Channel &ch : mode.channels)
        if (starts_with(ch.attribute(), "ColorAdd_") &&
            std::ranges::find(colour_geometries, ch.geometry) == colour_geometries.end())
            colour_geometries.push_back(ch.geometry);
    const bool multi_cell = colour_geometries.size() > 1;

    for (const Channel &ch : mode.channels) {
        const auto [fn, p, slot] = decode_channel(ch, universe, base);
        const std::string_view attr = fn->attribute;
        const float pf = float(std::clamp(p, 0.0, 1.0));
        if (attr == "Dimmer")
            a.dimmer = std::clamp(p, 0.0, 1.0), has_dimmer = true;
        else if (attr == "Pan")
            a.pan = p;
        else if (attr == "Tilt")
            a.tilt = p;
        else if (attr == "Zoom")
            a.beam_angle = p;
        else if (starts_with(attr, "Focus"))
            a.focus = pf;
        else if (attr == "Iris")
            a.iris = pf;
        else if (starts_with(attr, "Frost"))
            a.frost = pf;
        else if (starts_with(attr, "ColorAdd_")) {
            const std::string_view c = attr.substr(9);
            Rgb &target = multi_cell ? cell(ch.geometry) : add;
            has_add = true;
            if (c == "R")
                target[0] += pf;
            else if (c == "G")
                target[1] += pf;
            else if (c == "B")
                target[2] += pf;
            else if (c == "W")
                for (float &k : target)
                    k += pf;
        } else if (starts_with(attr, "Color") && !fn->wheel.empty()) {
            const Wheel *w = type.wheel(fn->wheel);
            int s = slot;
            if (s == 0 && w && !w->slots.empty()) // no ChannelSets: physical value is the slot number
                s = std::clamp(int(std::lround(p)), 1, int(w->slots.size()));
            a.colour_slot = s;
            if (w && s >= 1 && s <= int(w->slots.size()))
                filter = w->slots[std::size_t(s - 1)].colour;
        } else if (starts_with(attr, "Gobo") && !fn->wheel.empty()) {
            const Wheel *w = type.wheel(fn->wheel);
            a.gobo_slot =
                slot ? slot : (w && !w->slots.empty() ? std::clamp(int(std::lround(p)), 1, int(w->slots.size())) : 0);
        } else if (starts_with(attr, "Shutter")) {
            // "Shutter1" open/closed, "Shutter1Strobe*" strobe with physical Hz.
            if (attr.find("Strobe") != std::string_view::npos)
                a.strobe_hz = std::max(p, 0.0);
            else if (fn->name.find("Closed") != std::string::npos)
                a.shutter_open = false;
        }
    }
    if (!has_dimmer)
        a.dimmer = 1;
    Rgb base_colour = has_add && !multi_cell ? add : type.emitter;
    for (std::size_t i = 0; i < 3; ++i)
        a.colour[i] = std::min(base_colour[i], 1.0f) * filter[i];
    for (Rgb &c : a.cells)
        for (float &k : c)
            k = std::min(k, 1.0f);
    return a;
}

std::vector<std::uint16_t> intensity_offsets(const Mode &mode) {
    std::vector<std::uint16_t> out;
    for (const Channel &ch : mode.channels)
        if (ch.attribute() == "Dimmer")
            out.insert(out.end(), ch.offsets.begin(), ch.offsets.end());
    return out;
}

} // namespace sv::gdtf
