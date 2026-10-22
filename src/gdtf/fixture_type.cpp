#include "gdtf/fixture_type.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <map>
#include <miniz.h>
#include <pugixml.hpp>

namespace sv::gdtf {

namespace {

constexpr std::size_t max_description_size = 64u << 20;

double num(const pugi::xml_node &n, const char *attr, double fallback) {
    const char *s = n.attribute(attr).value();
    char *end = nullptr;
    const double v = std::strtod(s, &end);
    return end == s ? fallback : v;
}

// "128/1" -> value at the channel's byte resolution (GDTF DMXValue).
std::uint32_t dmx_value_attr(const char *s, std::size_t channel_bytes) {
    std::string_view v(s);
    if (v.empty())
        return 0;
    const auto slash = v.find('/');
    std::uint32_t value = 0, bytes = 1;
    std::from_chars(v.data(), v.data() + std::min(slash, v.size()), value);
    if (slash != std::string_view::npos)
        std::from_chars(v.data() + slash + 1, v.data() + v.size(), bytes);
    if (bytes < 1 || bytes > 4)
        return 0;
    if (bytes < channel_bytes)
        value <<= 8 * (channel_bytes - bytes);
    else if (bytes > channel_bytes)
        value >>= 8 * (bytes - channel_bytes);
    return value;
}

Rgb colour_attr(const char *s) {
    double x = 0.3127, y = 0.3290;
    if (std::sscanf(s, "%lf,%lf", &x, &y) != 2)
        return {1, 1, 1};
    return cie_to_rgb(x, y);
}

const pugi::xml_node find_beam(const pugi::xml_node &n) {
    for (auto c : n.children()) {
        if (std::string_view(c.name()) == "Beam")
            return c;
        if (auto b = find_beam(c))
            return b;
    }
    return {};
}

} // namespace

Rgb cie_to_rgb(double x, double y) {
    if (y <= 0)
        return {1, 1, 1};
    const double X = x / y, Y = 1, Z = (1 - x - y) / y;
    double r = 3.2406 * X - 1.5372 * Y - 0.4986 * Z;
    double g = -0.9689 * X + 1.8758 * Y + 0.0415 * Z;
    double b = 0.0557 * X - 0.2040 * Y + 1.0570 * Z;
    r = std::max(r, 0.0), g = std::max(g, 0.0), b = std::max(b, 0.0);
    const double m = std::max({r, g, b, 1e-9});
    return {float(r / m), float(g / m), float(b / m)};
}

const Mode *FixtureType::mode(std::string_view n) const {
    auto it = std::ranges::find(modes, n, &Mode::name);
    return it == modes.end() ? nullptr : &*it;
}

const Wheel *FixtureType::wheel(std::string_view n) const {
    auto it = std::ranges::find(wheels, n, &Wheel::name);
    return it == wheels.end() ? nullptr : &*it;
}

FixtureType parse_description(std::string_view xml) {
    pugi::xml_document doc;
    if (!doc.load_buffer(xml.data(), xml.size()))
        throw GdtfError("description.xml is not valid XML");
    const auto ft = doc.child("GDTF").child("FixtureType");
    if (!ft)
        throw GdtfError("description.xml has no GDTF/FixtureType");

    FixtureType t;
    t.name = ft.attribute("Name").value();
    t.short_name = ft.attribute("ShortName").value();
    t.manufacturer = ft.attribute("Manufacturer").value();
    t.id = ft.attribute("FixtureTypeID").value();

    for (auto w : ft.child("Wheels").children("Wheel")) {
        Wheel wheel{w.attribute("Name").value(), {}};
        for (auto s : w.children("Slot"))
            wheel.slots.push_back(
                {s.attribute("Name").value(), colour_attr(s.attribute("Color").as_string("0.3127,0.3290"))});
        t.wheels.push_back(std::move(wheel));
    }
    if (auto e = ft.child("PhysicalDescriptions").child("Emitters").child("Emitter"))
        t.emitter = colour_attr(e.attribute("Color").value());
    if (auto m = ft.child("Models").child("Model"))
        t.body = {num(m, "Length", 0.3), num(m, "Height", 0.4), num(m, "Width", 0.3)};
    if (auto b = find_beam(ft.child("Geometries"))) {
        t.beam_angle = num(b, "BeamAngle", t.beam_angle);
        t.field_angle = num(b, "FieldAngle", t.field_angle);
        t.luminous_flux = num(b, "LuminousFlux", t.luminous_flux);
        t.beam_radius = num(b, "BeamRadius", t.beam_radius);
    }

    for (auto m : ft.child("DMXModes").children("DMXMode")) {
        Mode mode{m.attribute("Name").value(), {}, 0};
        for (auto c : m.child("DMXChannels").children("DMXChannel")) {
            Channel ch;
            const std::string_view offset = c.attribute("Offset").value();
            if (offset.empty() || offset == "None")
                continue; // virtual channel
            for (std::size_t pos = 0; pos < offset.size();) {
                const auto comma = std::min(offset.find(',', pos), offset.size());
                int o = 0;
                std::from_chars(offset.data() + pos, offset.data() + comma, o);
                if (o < 1 || o > 512)
                    throw GdtfError("mode " + mode.name + ": bad channel offset");
                ch.offsets.push_back(std::uint16_t(o - 1));
                pos = comma + 1;
            }
            if (ch.offsets.size() > 4)
                throw GdtfError("mode " + mode.name + ": channel wider than 32 bits");
            // shortcut: DMXBreak > 1 (second DMX link) is folded into the first footprint, real multi-break
            // fixtures are rare in the alpha library.
            ch.geometry = c.attribute("Geometry").value();
            const std::size_t bytes = ch.offsets.size();
            for (auto lc : c.children("LogicalChannel")) {
                for (auto f : lc.children("ChannelFunction")) {
                    ChannelFunction fn;
                    fn.name = f.attribute("Name").value();
                    fn.attribute = f.attribute("Attribute").as_string(lc.attribute("Attribute").value());
                    fn.dmx_from = dmx_value_attr(f.attribute("DMXFrom").as_string("0/1"), bytes);
                    fn.physical_from = num(f, "PhysicalFrom", 0);
                    fn.physical_to = num(f, "PhysicalTo", 1);
                    fn.wheel = f.attribute("Wheel").value();
                    for (auto s : f.children("ChannelSet"))
                        fn.sets.push_back({dmx_value_attr(s.attribute("DMXFrom").as_string("0/1"), bytes),
                                           s.attribute("WheelSlotIndex").as_int(0)});
                    if (!f.attribute("Default").empty())
                        ch.default_value = dmx_value_attr(f.attribute("Default").value(), bytes);
                    ch.functions.push_back(std::move(fn));
                }
            }
            if (ch.functions.empty())
                continue;
            std::ranges::stable_sort(ch.functions, {}, &ChannelFunction::dmx_from);
            if (auto d = c.attribute("Default"); !d.empty()) // GDTF 1.0 puts Default on the channel
                ch.default_value = dmx_value_attr(d.value(), bytes);
            for (auto o : ch.offsets)
                mode.footprint = std::max<std::uint16_t>(mode.footprint, std::uint16_t(o + 1));
            mode.channels.push_back(std::move(ch));
        }
        t.modes.push_back(std::move(mode));
    }
    if (t.modes.empty())
        throw GdtfError("fixture type " + t.name + " has no DMX modes");
    return t;
}

FixtureType load_gdtf(std::span<const std::uint8_t> bytes) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0))
        throw GdtfError("not a GDTF package (zip)");
    std::string xml;
    const int index = mz_zip_reader_locate_file(&zip, "description.xml", nullptr, 0);
    mz_zip_archive_file_stat st;
    if (index >= 0 && mz_zip_reader_file_stat(&zip, mz_uint(index), &st) && st.m_uncomp_size <= max_description_size) {
        xml.resize(std::size_t(st.m_uncomp_size));
        if (!mz_zip_reader_extract_to_mem(&zip, mz_uint(index), xml.data(), xml.size(), 0))
            xml.clear();
    }
    mz_zip_reader_end(&zip);
    if (xml.empty())
        throw GdtfError("GDTF package has no readable description.xml");
    return parse_description(xml);
}

FixtureType load_gdtf(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw GdtfError("cannot open " + path.string());
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), {}};
    return load_gdtf(bytes);
}

} // namespace sv::gdtf
