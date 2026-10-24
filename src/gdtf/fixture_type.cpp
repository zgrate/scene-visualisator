#include "gdtf/fixture_type.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <miniz.h>
#include <pugixml.hpp>
#include <sstream>

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

// Every geometry name below a top-level geometry -> that top-level name (GeometryReference targets).
void map_tops(const pugi::xml_node &n, const std::string &top, std::map<std::string, std::string> &out) {
    for (auto c : n.children()) {
        if (const char *name = c.attribute("Name").value(); *name)
            out.emplace(name, top);
        map_tops(c, top, out);
    }
}

// One instance of a referenced geometry: its name and the DMXOffset of each Break (key 0 = the last, "Overwrite").
struct GeometryInstance {
    std::string name;
    std::map<int, int> breaks;
};

void find_references(const pugi::xml_node &n, std::map<std::string, std::vector<GeometryInstance>> &out) {
    for (auto c : n.children()) {
        if (std::string_view(c.name()) == "GeometryReference") {
            GeometryInstance g{c.attribute("Name").value(), {}};
            for (auto b : c.children("Break")) {
                g.breaks[b.attribute("DMXBreak").as_int(1)] = b.attribute("DMXOffset").as_int(1);
                g.breaks[0] = b.attribute("DMXOffset").as_int(1);
            }
            out[c.attribute("Geometry").value()].push_back(std::move(g));
        }
        find_references(c, out);
    }
}

using Mat3 = std::array<std::array<double, 3>, 3>;
using Vec3 = std::array<double, 3>;

// Rotation part of a GDTF Position "{u1,u2,u3,o1}{v1,v2,v3,o2}{w1,w2,w3,o3}{0,0,0,1}"; identity when absent.
// The translation (o1, o2, o3) goes to `offset` when given.
Mat3 rotation_of(const pugi::xml_node &g, Vec3 *offset = nullptr) {
    Mat3 r{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    Vec3 o{0, 0, 0};
    const std::string_view s = g.attribute("Position").value();
    std::size_t at = 0;
    for (int row = 0; row < 3; ++row) {
        const auto open = s.find('{', at);
        if (open == std::string_view::npos)
            return {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
        std::istringstream in(std::string(s.substr(open + 1, s.find('}', open) - open - 1)));
        char comma;
        in >> r[row][0] >> comma >> r[row][1] >> comma >> r[row][2] >> comma >> o[std::size_t(row)];
        at = open + 1;
    }
    if (offset)
        *offset = o;
    return r;
}

Mat3 multiply(const Mat3 &a, const Mat3 &b) {
    Mat3 m{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                m[i][j] += a[i][k] * b[k][j];
    return m;
}

// First Beam below n, with the rotations of every geometry on the way to it composed into r.
pugi::xml_node find_beam(const pugi::xml_node &n, Mat3 &r) {
    for (auto c : n.children()) {
        const Mat3 here = multiply(r, rotation_of(c));
        if (std::string_view(c.name()) == "Beam")
            return r = here, c;
        Mat3 below = here;
        if (auto b = find_beam(c, below))
            return r = below, b;
    }
    return {};
}

// Head candidates below n: the first Axis on each path opens one at its composed pivot, and every geometry name
// below it (GeometryReference instances included) maps to that candidate.
void find_heads(const pugi::xml_node &n, const Mat3 &r, const Vec3 &o, int head, std::vector<Head> &heads,
                std::map<std::string, int> &head_of) {
    for (auto c : n.children()) {
        const char *name = c.attribute("Name").value();
        if (!*name)
            continue;
        Vec3 local{}, here = o;
        const Mat3 rot = rotation_of(c, &local);
        for (std::size_t i = 0; i < 3; ++i)
            for (std::size_t k = 0; k < 3; ++k)
                here[i] += r[i][k] * local[k];
        int h = head;
        if (h < 0 && std::string_view(c.name()) == "Axis") {
            h = int(heads.size());
            heads.push_back({{here[0], here[2], -here[1]}}); // GDTF Z up -> ours Y up
        }
        if (h >= 0)
            head_of.emplace(name, h);
        find_heads(c, multiply(r, rot), here, h, heads, head_of);
    }
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

std::array<double, 2> rgb_to_cie(const Rgb &c) {
    const double X = 0.4124 * c[0] + 0.3576 * c[1] + 0.1805 * c[2];
    const double Y = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
    const double Z = 0.0193 * c[0] + 0.1192 * c[1] + 0.9505 * c[2];
    const double sum = X + Y + Z;
    return sum <= 0 ? std::array{0.3127, 0.3290} : std::array{X / sum, Y / sum};
}

namespace {
std::string colour_text(const Rgb &c) {
    const auto [x, y] = rgb_to_cie(c);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6f,%.6f,100", x, y);
    return buf;
}
std::string dmx_text(std::uint32_t value, std::size_t bytes) {
    return std::to_string(value) + "/" + std::to_string(bytes);
}
} // namespace

std::string write_description(const FixtureType &t) {
    pugi::xml_document doc;
    auto decl = doc.append_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "UTF-8";
    auto gdtf = doc.append_child("GDTF");
    gdtf.append_attribute("DataVersion") = "1.2";
    auto ft = gdtf.append_child("FixtureType");
    ft.append_attribute("Name") = t.name.c_str();
    ft.append_attribute("ShortName") = t.short_name.c_str();
    ft.append_attribute("LongName") = t.name.c_str();
    ft.append_attribute("Manufacturer") = t.manufacturer.c_str();
    ft.append_attribute("Description") = "Made in Scene Visualiser";
    ft.append_attribute("FixtureTypeID") = t.id.c_str();
    ft.append_attribute("RefFT") = "";
    ft.append_attribute("Thumbnail") = "";
    ft.append_child("AttributeDefinitions");
    auto wheels = ft.append_child("Wheels");
    for (const Wheel &w : t.wheels) {
        auto wn = wheels.append_child("Wheel");
        wn.append_attribute("Name") = w.name.c_str();
        for (const WheelSlot &slot : w.slots) {
            auto sn = wn.append_child("Slot");
            sn.append_attribute("Name") = slot.name.c_str();
            sn.append_attribute("Color") = colour_text(slot.colour).c_str();
        }
    }
    auto emitter = ft.append_child("PhysicalDescriptions").append_child("Emitters").append_child("Emitter");
    emitter.append_attribute("Name") = "Emitter";
    emitter.append_attribute("Color") = colour_text(t.emitter).c_str();
    auto model = ft.append_child("Models").append_child("Model");
    model.append_attribute("Name") = "Body";
    model.append_attribute("Length") = t.body[0];
    model.append_attribute("Height") = t.body[1];
    model.append_attribute("Width") = t.body[2];
    model.append_attribute("PrimitiveType") = "Cube";
    auto base = ft.append_child("Geometries").append_child("Geometry");
    base.append_attribute("Name") = "Base";
    base.append_attribute("Model") = "Body";
    auto yoke = base.append_child("Axis");
    yoke.append_attribute("Name") = "Yoke";
    auto head = yoke.append_child("Axis");
    head.append_attribute("Name") = "Head";
    auto beam = head.append_child("Beam");
    beam.append_attribute("Name") = "Beam";
    beam.append_attribute("Model") = "Body";
    beam.append_attribute("BeamAngle") = t.beam_angle;
    beam.append_attribute("FieldAngle") = t.field_angle;
    beam.append_attribute("LuminousFlux") = t.luminous_flux;
    beam.append_attribute("BeamRadius") = t.beam_radius;
    beam.append_attribute("BeamType") = "Spot";

    auto modes = ft.append_child("DMXModes");
    for (const Mode &m : t.modes) {
        auto mn = modes.append_child("DMXMode");
        mn.append_attribute("Name") = m.name.c_str();
        mn.append_attribute("Geometry") = "Base";
        auto channels = mn.append_child("DMXChannels");
        for (const Channel &ch : m.channels) {
            const std::size_t bytes = ch.offsets.size();
            std::string offsets;
            for (auto o : ch.offsets)
                offsets += (offsets.empty() ? "" : ",") + std::to_string(o + 1);
            const std::string &attr = ch.attribute();
            auto cn = channels.append_child("DMXChannel");
            cn.append_attribute("DMXBreak") = "1";
            cn.append_attribute("Offset") = offsets.c_str();
            cn.append_attribute("Geometry") = attr == "Pan" ? "Yoke" : attr == "Tilt" ? "Head" : "Beam";
            auto lc = cn.append_child("LogicalChannel");
            lc.append_attribute("Attribute") = attr.c_str();
            for (std::size_t i = 0; i < ch.functions.size(); ++i) {
                const ChannelFunction &f = ch.functions[i];
                auto fn = lc.append_child("ChannelFunction");
                fn.append_attribute("Name") = (f.name.empty() ? f.attribute : f.name).c_str();
                fn.append_attribute("Attribute") = f.attribute.c_str();
                fn.append_attribute("DMXFrom") = dmx_text(f.dmx_from, bytes).c_str();
                if (i == 0)
                    fn.append_attribute("Default") = dmx_text(ch.default_value, bytes).c_str();
                fn.append_attribute("PhysicalFrom") = f.physical_from;
                fn.append_attribute("PhysicalTo") = f.physical_to;
                if (!f.wheel.empty())
                    fn.append_attribute("Wheel") = f.wheel.c_str();
                for (const ChannelSet &set : f.sets) {
                    auto sn = fn.append_child("ChannelSet");
                    sn.append_attribute("DMXFrom") = dmx_text(set.dmx_from, bytes).c_str();
                    sn.append_attribute("WheelSlotIndex") = set.wheel_slot;
                }
            }
        }
        mn.append_child("Relations");
        mn.append_child("FTMacros");
    }
    std::ostringstream out;
    doc.save(out, "", pugi::format_raw);
    return out.str();
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
    Mat3 rot{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    const auto b = find_beam(ft.child("Geometries"), rot);
    t.has_beam = b || !ft.child("Geometries").first_child(); // a file without geometry says nothing: assume a lamp
    if (b) {
        // The beam shines along its local -Z; GDTF is Z up, ours Y up: (x, y, z) -> (x, z, -y).
        t.beam_direction = {-rot[0][2], -rot[2][2], rot[1][2]};
        t.beam_angle = num(b, "BeamAngle", t.beam_angle);
        t.field_angle = num(b, "FieldAngle", t.field_angle);
        t.luminous_flux = num(b, "LuminousFlux", t.luminous_flux);
        t.beam_radius = num(b, "BeamRadius", t.beam_radius);
    }

    std::map<std::string, std::string> top_of;
    for (auto g : ft.child("Geometries").children()) {
        const std::string name = g.attribute("Name").value();
        top_of.emplace(name, name);
        map_tops(g, name, top_of);
    }

    for (auto m : ft.child("DMXModes").children("DMXMode")) {
        Mode mode{m.attribute("Name").value(), {}, 0};
        // Channels on a referenced geometry are a template, repeated once per GeometryReference in this mode's tree
        // with offsets relative to that reference's Break.
        std::map<std::string, std::vector<GeometryInstance>> references;
        for (auto g : ft.child("Geometries").children())
            if (std::string_view(g.attribute("Name").value()) == m.attribute("Geometry").value())
                find_references(g, references);
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
            const auto top = top_of.find(ch.geometry);
            const auto refs = top == top_of.end() ? references.end() : references.find(top->second);
            if (refs == references.end()) {
                for (auto o : ch.offsets)
                    mode.footprint = std::max<std::uint16_t>(mode.footprint, std::uint16_t(o + 1));
                mode.channels.push_back(std::move(ch));
                continue;
            }
            const std::string_view brk = c.attribute("DMXBreak").as_string("1");
            const int break_no = brk == "Overwrite" ? 0 : std::atoi(brk.data());
            for (const GeometryInstance &g : refs->second) {
                const auto b = g.breaks.find(break_no);
                if (b == g.breaks.end())
                    continue;
                Channel copy = ch;
                copy.geometry = g.name;
                for (auto &o : copy.offsets) {
                    const int shifted = o + b->second - 1;
                    if (shifted < 0 || shifted > 511)
                        throw GdtfError("mode " + mode.name + ": referenced channel past 512");
                    o = std::uint16_t(shifted);
                    mode.footprint = std::max<std::uint16_t>(mode.footprint, std::uint16_t(o + 1));
                }
                mode.channels.push_back(std::move(copy));
            }
        }
        // Heads are the Axis candidates that carry Pan or Tilt, numbered in channel order.
        std::vector<Head> candidates;
        std::map<std::string, int> head_of;
        for (auto g : ft.child("Geometries").children())
            if (std::string_view(g.attribute("Name").value()) == m.attribute("Geometry").value())
                find_heads(g, {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}, {0, 0, 0}, -1, candidates, head_of);
        std::vector<int> renumber(candidates.size(), -1);
        for (const Channel &ch : mode.channels)
            if (const auto it = head_of.find(ch.geometry); it != head_of.end() &&
                                                           (ch.attribute() == "Pan" || ch.attribute() == "Tilt") &&
                                                           renumber[std::size_t(it->second)] < 0) {
                renumber[std::size_t(it->second)] = int(mode.heads.size());
                mode.heads.push_back(candidates[std::size_t(it->second)]);
            }
        for (Channel &ch : mode.channels)
            if (const auto it = head_of.find(ch.geometry); it != head_of.end())
                ch.head = renumber[std::size_t(it->second)];
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
