#include "gdtf/decoder.h"

#include "gdtf/dmx_value.h"

#include <algorithm>
#include <cmath>
#include <optional>
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

// Blackbody colour of a Kelvin temperature, 0..1 per channel (Tanner Helland's fit, good from 1000 K to 40000 K).
Rgb kelvin_rgb(double kelvin) {
    const double t = std::clamp(kelvin, 1000.0, 40000.0) / 100;
    const double r = t <= 66 ? 255 : 329.698727446 * std::pow(t - 60, -0.1332047592);
    const double g =
        t <= 66 ? 99.4708025861 * std::log(t) - 161.1195681661 : 288.1221695283 * std::pow(t - 60, -0.0755148492);
    const double b = t >= 66 ? 255 : t <= 19 ? 0 : 138.5177312231 * std::log(t - 10) - 305.0447927307;
    return {float(std::clamp(r / 255, 0.0, 1.0)), float(std::clamp(g / 255, 0.0, 1.0)),
            float(std::clamp(b / 255, 0.0, 1.0))};
}

} // namespace

namespace {

// Approximate colour of each GDTF additive emitter (ColorAdd_<name>).
const Rgb *add_emitter(std::string_view name) {
    static constexpr std::pair<std::string_view, Rgb> table[] = {
        {"R", {1, 0, 0}},         {"G", {0, 1, 0}},          {"B", {0, 0, 1}},     {"W", {1, 1, 1}},
        {"WW", {1, 0.8f, 0.55f}}, {"CW", {0.85f, 0.92f, 1}}, {"A", {1, 0.75f, 0}}, {"UV", {0.3f, 0, 0.6f}},
        {"C", {0, 1, 1}},         {"M", {1, 0, 1}},          {"Y", {1, 1, 0}},     {"RY", {1, 0.5f, 0}},
        {"GY", {0.5f, 1, 0}},     {"GC", {0, 1, 0.5f}},      {"BC", {0, 0.5f, 1}}, {"BM", {0.5f, 0, 1}},
        {"RM", {1, 0, 0.5f}},
    };
    for (const auto &[n, rgb] : table)
        if (n == name)
            return &rgb;
    return nullptr;
}

} // namespace

Attributes decode(const FixtureType &type, const Mode &mode, std::span<const std::uint8_t> universe,
                  std::uint16_t address) {
    Attributes a;
    a.beam_angle = type.beam_angle;
    const std::size_t base = address >= 1 ? address - 1 : 0;
    Rgb add{0, 0, 0}, filter{1, 1, 1};
    bool has_add = false, has_dimmer = false;
    std::vector<std::string_view> cell_geometry;
    std::vector<int> cell_head;
    auto cell = [&](const Channel &ch) -> Rgb & {
        auto it = std::ranges::find(cell_geometry, ch.geometry);
        if (it == cell_geometry.end()) {
            cell_geometry.push_back(ch.geometry);
            cell_head.push_back(ch.head);
            a.cells.push_back({0, 0, 0});
            return a.cells.back();
        }
        return a.cells[std::size_t(it - cell_geometry.begin())];
    };
    if (mode.heads.size() > 1)
        a.heads.assign(mode.heads.size(), {0, 0, type.beam_angle, {0, 0, 0}});
    // Pan, Tilt and Zoom of a multi-head fixture also go to their head.
    auto head_of = [&](const Channel &ch) {
        return ch.head >= 0 && !a.heads.empty() ? &a.heads[std::size_t(ch.head)] : nullptr;
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
        if (attr == "Dimmer") {
            // The first Dimmer is the master; later ones are sub-dimmers (K10 rings, TORNADO arrays, strobe beams).
            if (!has_dimmer)
                a.dimmer = std::clamp(p, 0.0, 1.0);
            has_dimmer = true;
        } else if (attr == "Pan") {
            a.pan = p;
            if (HeadState *h = head_of(ch))
                h->pan = p;
        } else if (attr == "Tilt") {
            a.tilt = p;
            if (HeadState *h = head_of(ch))
                h->tilt = p;
        } else if (attr == "Zoom") {
            a.beam_angle = p;
            if (HeadState *h = head_of(ch))
                h->beam_angle = p;
        } else if (starts_with(attr, "Focus"))
            a.focus = pf;
        else if (attr == "Iris")
            a.iris = pf;
        else if (starts_with(attr, "Frost"))
            a.frost = pf;
        else if ((starts_with(attr, "Fog") || starts_with(attr, "Haze")) && attr != "FogType")
            a.haze = std::max(a.haze, double(pf));
        else if (starts_with(attr, "ColorAdd_")) {
            Rgb &target = multi_cell ? cell(ch) : add;
            has_add = true;
            if (const Rgb *e = add_emitter(attr.substr(9)))
                for (std::size_t i = 0; i < 3; ++i)
                    target[i] += pf * (*e)[i];
        } else if (starts_with(attr, "ColorSub_")) { // CMY flags: cyan takes red, magenta green, yellow blue
            const std::size_t i = std::string_view("CMY").find(attr.substr(9));
            if (attr.size() == 10 && i != std::string_view::npos)
                filter[i] *= 1 - pf;
        } else if (attr == "CTO" || attr == "CTB" || attr == "CTC") {
            // Physical values are Kelvin; the filter turns the function's starting temperature into the current one.
            const Rgb from = kelvin_rgb(fn->physical_from), to = kelvin_rgb(p);
            Rgb ratio{};
            for (std::size_t i = 0; i < 3; ++i)
                ratio[i] = to[i] / std::max(from[i], 0.01f);
            const float top = std::max({ratio[0], ratio[1], ratio[2], 0.01f});
            for (std::size_t i = 0; i < 3; ++i)
                filter[i] *= ratio[i] / top;
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
            const int s =
                slot ? slot : (w && !w->slots.empty() ? std::clamp(int(std::lround(p)), 1, int(w->slots.size())) : 0);
            if (s > 1 || a.gobo_slot == 0) // a second wheel at slot 1 (open) keeps the first wheel's gobo
                a.gobo_slot = s;
        } else if (starts_with(attr, "Shutter")) {
            // "Shutter1" open/closed, "Shutter1Strobe*" strobe with physical Hz.
            if (attr.find("Strobe") != std::string_view::npos)
                a.strobe_hz = std::max(p, 0.0);
            else if (fn->name.find("Closed") != std::string::npos)
                a.shutter_open = false;
        }
    }
    // No Dimmer channel: colour or a fixed lamp carries the intensity, unless there is no Beam at all (bug 9).
    if (!has_dimmer)
        a.dimmer = type.has_beam ? 1 : 0;
    for (Rgb &c : a.cells)
        for (float &k : c)
            k = std::min(k, 1.0f);
    // Multi-cell fixtures (TORNADO, K10) colour their single beam with the mean of their cells.
    Rgb base_colour = !has_add ? type.emitter : add;
    if (has_add && multi_cell) {
        base_colour = {0, 0, 0};
        for (const Rgb &c : a.cells)
            for (std::size_t i = 0; i < 3; ++i)
                base_colour[i] += c[i] / float(a.cells.size());
    }
    for (std::size_t i = 0; i < 3; ++i)
        a.colour[i] = std::min(base_colour[i], 1.0f) * filter[i];
    // Each head takes the mean of its own cells; a head without cells shows the fixture colour.
    std::vector<int> count(a.heads.size(), 0);
    for (std::size_t c = 0; c < a.cells.size(); ++c)
        if (cell_head[c] >= 0 && std::size_t(cell_head[c]) < a.heads.size()) {
            ++count[std::size_t(cell_head[c])];
            for (std::size_t i = 0; i < 3; ++i)
                a.heads[std::size_t(cell_head[c])].colour[i] += a.cells[c][i];
        }
    for (std::size_t h = 0; h < a.heads.size(); ++h)
        for (std::size_t i = 0; i < 3; ++i)
            a.heads[h].colour[i] = count[h] ? a.heads[h].colour[i] / float(count[h]) * filter[i] : a.colour[i];
    return a;
}

namespace {

void put(std::vector<DmxByte> &out, const Channel &ch, std::uint32_t v) {
    const std::size_t n = ch.offsets.size();
    for (std::size_t i = 0; i < n; ++i)
        out.push_back({ch.offsets[i], std::uint8_t(v >> (8 * (n - 1 - i)))});
}

std::uint32_t function_end(const Channel &ch, std::size_t f) {
    const std::uint32_t max = std::uint32_t((std::uint64_t{1} << (8 * ch.offsets.size())) - 1);
    return f + 1 < ch.functions.size() ? ch.functions[f + 1].dmx_from - 1 : max;
}

} // namespace

std::vector<DmxByte> encode(const Mode &mode, std::string_view attribute, double value, int head) {
    std::vector<DmxByte> out;
    for (const Channel &ch : mode.channels) {
        if (head >= 0 && ch.head != head)
            continue;
        // The first function of the attribute whose physical range holds the value, else its first.
        std::optional<std::size_t> pick;
        for (std::size_t f = 0; f < ch.functions.size(); ++f) {
            const ChannelFunction &fn = ch.functions[f];
            if (fn.attribute != attribute)
                continue;
            const bool holds = std::min(fn.physical_from, fn.physical_to) <= value &&
                               value <= std::max(fn.physical_from, fn.physical_to);
            if (!pick || holds)
                pick = f;
            if (holds)
                break;
        }
        if (!pick)
            continue;
        const ChannelFunction &fn = ch.functions[*pick];
        const auto set = std::ranges::find(fn.sets, int(std::lround(value)), &ChannelSet::wheel_slot);
        if (set != fn.sets.end()) {
            put(out, ch, set->dmx_from);
            continue;
        }
        const std::uint32_t to = function_end(ch, *pick);
        const double span = fn.physical_to - fn.physical_from;
        const double k = span == 0 ? 0 : std::clamp((value - fn.physical_from) / span, 0.0, 1.0);
        put(out, ch, fn.dmx_from + std::uint32_t(std::lround(k * double(to - fn.dmx_from))));
    }
    return out;
}

std::vector<DmxByte> open_shutter(const Mode &mode) {
    std::vector<DmxByte> out;
    for (const Channel &ch : mode.channels) {
        const ChannelFunction *open = nullptr;
        for (const ChannelFunction &fn : ch.functions)
            if (starts_with(fn.attribute, "Shutter") && fn.name.find("Open") != std::string::npos) {
                open = &fn;
                break;
            }
        for (const ChannelFunction &fn : ch.functions)
            if (!open && starts_with(fn.attribute, "Shutter") && fn.attribute.find("Strobe") == std::string::npos &&
                fn.name.find("Closed") == std::string::npos)
                open = &fn;
        if (open)
            put(out, ch, open->dmx_from);
    }
    return out;
}

std::vector<std::uint16_t> intensity_offsets(const Mode &mode) {
    std::vector<std::uint16_t> out;
    for (const Channel &ch : mode.channels)
        if (ch.attribute() == "Dimmer")
            out.insert(out.end(), ch.offsets.begin(), ch.offsets.end());
    return out;
}

bool continuous_attribute(std::string_view a) {
    if (a.find("Mode") != std::string_view::npos || a.find("Rotate") != std::string_view::npos)
        return false;
    for (std::string_view exact : {"Dimmer", "Pan", "Tilt", "CTO", "CTB", "CTC", "Zoom", "Iris"})
        if (a == exact)
            return true;
    for (std::string_view prefix : {"ColorAdd_", "ColorSub_", "ColorRGB_", "Focus", "Frost"})
        if (a.starts_with(prefix))
            return true;
    return false;
}

} // namespace sv::gdtf

