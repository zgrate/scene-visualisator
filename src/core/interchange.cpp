#include "core/interchange.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <pugixml.hpp>
#include <sstream>

namespace sv {

namespace {

std::string num(double v) { // two decimals, shortest form: 1.5, -4.5, 0
    v = std::round(v * 100) / 100;
    if (v == 0)
        v = 0; // no "-0"
    char buf[32];
    return {buf, std::to_chars(buf, buf + sizeof buf, v).ptr};
}

std::string esc(std::string_view s) {
    std::string out;
    for (char c : s)
        switch (c) {
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '&':
            out += "&amp;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
        }
    return out;
}

std::string csv_quote(std::string_view s) {
    std::string out = "\"";
    for (char c : s)
        out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + '"';
}

std::string type_name(const json &f) { // GDTF file name without the extension
    std::string t = f.value("gdtf", std::string());
    if (t.ends_with(".gdtf"))
        t.resize(t.size() - 5);
    return t;
}

std::string patch_text(const json &p) {
    if (!p.is_object())
        return {};
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d.%03d", p.value("universe", 1), p.value("address", 1));
    return buf;
}

std::string id_text(const json &f) {
    const json &id = f.value("fixtureId", json());
    return id.is_number_integer() ? std::to_string(id.get<int>()) : std::string();
}

std::vector<const json *> of_type(const json &project, std::string_view type) {
    std::vector<const json *> out;
    if (!project.contains("objects")) // value() would return a temporary and leave the pointers dangling
        return out;
    for (const json &o : project.at("objects"))
        if (o.value("type", std::string()) == type)
            out.push_back(&o);
    return out;
}

double at(const json &o, const char *key, int i) {
    const json &a = o.value(key, json());
    return a.is_array() && a.size() > std::size_t(i) && a[i].is_number() ? a[i].get<double>() : 0.0;
}

} // namespace

std::string patch_csv(const json &project) {
    std::string out = "FixtureID,Name,FixtureType,Mode,Universe,Address,Patch,X,Y,Z,RotX,RotY,RotZ\n";
    for (const json *f : of_type(project, "fixture")) {
        const json &p = f->value("patch", json());
        out += id_text(*f) + ',' + csv_quote(f->value("name", std::string())) + ',' + csv_quote(type_name(*f)) + ',' +
               csv_quote(f->value("mode", std::string())) + ',' +
               (p.is_object() ? std::to_string(p.value("universe", 1)) + ',' + std::to_string(p.value("address", 1))
                              : std::string(",")) +
               ',' + patch_text(p);
        for (const char *key : {"position", "rotation"})
            for (int i = 0; i < 3; ++i)
                out += ',' + num(at(*f, key, i));
        out += '\n';
    }
    return out;
}

// shortcut: attribute names follow the publicly known grandMA3 patch export, confirm against a console export; the
// CSV is the safe path (docs "Interchange formats").
std::string ma3_patch_xml(const json &project) {
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<GMA3 DataVersion=\"2.1.0.0\">\n"
                      "  <Patch Name=\"Scene Visualiser\">\n    <Layer Name=\"Visualiser\">\n";
    for (const json *f : of_type(project, "fixture")) {
        const std::string id = id_text(*f);
        out += "      <Fixture Name=\"" + esc(f->value("name", std::string())) + "\" FixtureID=\"" + id +
               "\" CustomID=\"" + id + "\" FixtureType=\"" + esc(type_name(*f)) + "\" Mode=\"" +
               esc(f->value("mode", std::string())) + "\" Patch=\"" + patch_text(f->value("patch", json())) + "\">\n";
        out += "        <Position X=\"" + num(at(*f, "position", 0)) + "\" Y=\"" + num(at(*f, "position", 1)) +
               "\" Z=\"" + num(at(*f, "position", 2)) + "\"/>\n";
        out += "        <Rotation X=\"" + num(at(*f, "rotation", 0)) + "\" Y=\"" + num(at(*f, "rotation", 1)) +
               "\" Z=\"" + num(at(*f, "rotation", 2)) + "\"/>\n      </Fixture>\n";
    }
    return out + "    </Layer>\n  </Patch>\n</GMA3>\n";
}

// Resolume Arena 7 Advanced Output preset shape, reverse-engineered by the community, not an official schema.
// shortcut: one Slice per Screen with an identity warper and a virtual output device, verify in Arena before relying.
std::string resolume_xml(const json &project) {
    const json &comp = project.value("composition", json::object());
    const auto rect = [](int x, int y, int w, int h) {
        const auto v = [](int a, int b) {
            return "<v x=\"" + std::to_string(a) + "\" y=\"" + std::to_string(b) + "\"/>";
        };
        return v(x, y) + v(x + w, y) + v(x + w, y + h) + v(x, y + h);
    };
    std::string screens;
    int i = 0;
    for (const json *w : of_type(project, "ledWall")) {
        const std::string name = esc(w->value("name", std::string()));
        const WallDerived d = wall_derived(*w);
        const json &s = w->value("slice", json::object());
        screens += "    <Screen name=\"" + name + "\" uniqueId=\"" + std::to_string(1000 + i) + "\">\n" +
                   "      <Params><Param name=\"Name\" value=\"" + name +
                   "\"/><Param name=\"Enabled\" value=\"1\"/></Params>\n      <layers>\n" + "        <Slice name=\"" +
                   name + " slice\" uniqueId=\"" + std::to_string(2000 + i) + "\">\n" +
                   "          <Params><Param name=\"Name\" value=\"" + name +
                   " slice\"/><Param name=\"Enabled\" value=\"1\"/></Params>\n" + "          <InputRect>" +
                   rect(s.value("x", 0), s.value("y", 0), s.value("w", 0), s.value("h", 0)) + "</InputRect>\n" +
                   "          <OutputRect>" + rect(0, 0, d.pixels_x, d.pixels_y) + "</OutputRect>\n" +
                   "          <Warper><Homography identity=\"1\"/></Warper>\n        </Slice>\n      </layers>\n" +
                   "      <OutputDevice><OutputDeviceVirtual name=\"" + name + "\" width=\"" +
                   std::to_string(d.pixels_x) + "\" height=\"" + std::to_string(d.pixels_y) +
                   "\"/></OutputDevice>\n    </Screen>\n";
        ++i;
    }
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<XmlState name=\"Advanced Output\" version=\"7\">\n"
           "  <ScreenSetup name=\"Scene Visualiser\">\n    <CurrentCompositionTextureSize width=\"" +
           std::to_string(comp.value("width", 1920)) + "\" height=\"" + std::to_string(comp.value("height", 1080)) +
           "\"/>\n    <screens>\n" + screens + "    </screens>\n  </ScreenSetup>\n</XmlState>\n";
}

json resolume_import_patch(const json &project, std::string_view xml) {
    pugi::xml_document doc;
    if (const auto r = doc.load_buffer(xml.data(), xml.size()); !r)
        throw InterchangeError(std::string("XML parse error: ") + r.description());
    json patch = json::array();
    if (const auto size = doc.select_node("//CurrentCompositionTextureSize").node())
        for (const char *key : {"width", "height"})
            if (const int v = size.attribute(key).as_int(); v > 0)
                patch.push_back({{"op", "replace"}, {"path", std::string("/composition/") + key}, {"value", v}});
    const auto screens = doc.select_nodes("//Screen");
    if (screens.empty())
        throw InterchangeError("no <Screen> elements found");
    struct Rect {
        int x, y, w, h;
    };
    const auto bbox = [](pugi::xml_node rect) -> std::optional<Rect> {
        int x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
        for (pugi::xml_node v : rect.children("v")) {
            const int x = int(std::lround(v.attribute("x").as_double())),
                      y = int(std::lround(v.attribute("y").as_double()));
            x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
        }
        if (x0 > x1)
            return std::nullopt;
        return Rect{x0, y0, x1 - x0, y1 - y0};
    };
    const json &objects = project.value("objects", json::array());
    int added = 0, n = 0;
    for (const pugi::xpath_node &sn : screens) {
        const pugi::xml_node screen = sn.node();
        ++n;
        std::string name = screen.attribute("name").as_string();
        if (name.empty())
            name = screen.select_node("Params/Param[@name='Name']").node().attribute("value").as_string();
        if (name.empty())
            name = "Screen " + std::to_string(n);
        const pugi::xml_node slice = screen.select_node(".//Slice").node();
        const auto in = bbox(slice.child("InputRect"));
        if (!in)
            continue;
        const auto out = bbox(slice.child("OutputRect"));
        const json slice_json = {{"x", in->x}, {"y", in->y}, {"w", in->w}, {"h", in->h}};
        std::size_t i = 0;
        while (i < objects.size() && !(objects[i].value("type", std::string()) == "ledWall" &&
                                       objects[i].value("name", std::string()) == name))
            ++i;
        json wall = i < objects.size() ? objects[i] : new_led_wall(name);
        if (i == objects.size())
            wall["position"] = {-6.0 + (added++ % 5) * 3.0, 1.0, 0.0};
        wall["slice"] = slice_json;
        if (out && out->w > 0 && out->h > 0) { // native resolution x pitch, to the nearest half metre
            const double pitch = wall.value("pixelPitch", 3.9) / 1000;
            wall["width"] = std::max(0.5, std::round(out->w * pitch * 2) / 2);
            wall["height"] = std::max(0.5, std::round(out->h * pitch * 2) / 2);
        }
        if (i < objects.size())
            patch.push_back({{"op", "replace"}, {"path", "/objects/" + std::to_string(i)}, {"value", wall}});
        else
            patch.push_back({{"op", "add"}, {"path", "/objects/-"}, {"value", wall}});
    }
    return patch;
}

std::vector<ObjGroup> parse_obj(std::string_view text) {
    std::vector<std::array<float, 3>> v;
    std::vector<ObjGroup> groups;
    std::istringstream in{std::string(text)};
    std::string line;
    int line_no = 0;
    const auto group = [&]() -> ObjGroup & {
        if (groups.empty())
            groups.push_back({"default", {}});
        return groups.back();
    };
    while (std::getline(in, line)) {
        ++line_no;
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "v") {
            std::array<float, 3> p{};
            ls >> p[0] >> p[1] >> p[2];
            v.push_back(p);
        } else if (tag == "o" || tag == "g") {
            std::string name;
            std::getline(ls >> std::ws, name);
            if (!name.empty() && name.back() == '\r')
                name.pop_back();
            if (name.empty())
                name = "default";
            if (groups.empty() || groups.back().name != name) {
                if (!groups.empty() && groups.back().vertices.empty())
                    groups.back().name = name; // "o name" followed by "g name": one group
                else
                    groups.push_back({name, {}});
            }
        } else if (tag == "f") {
            std::vector<std::size_t> idx;
            std::string tok;
            while (ls >> tok) {
                const long i = std::strtol(tok.c_str(), nullptr, 10); // "7", "7/1", "7//3", "-1"
                const long resolved = i < 0 ? long(v.size()) + i : i - 1;
                if (i == 0 || resolved < 0 || resolved >= long(v.size()))
                    throw InterchangeError("OBJ line " + std::to_string(line_no) + ": bad vertex index " + tok);
                idx.push_back(std::size_t(resolved));
            }
            for (std::size_t k = 2; k < idx.size(); ++k) {
                const auto &a = v[idx[0]], &b = v[idx[k - 1]], &c = v[idx[k]];
                const std::array<float, 3> e1{b[0] - a[0], b[1] - a[1], b[2] - a[2]},
                    e2{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                std::array<float, 3> n{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                       e1[0] * e2[1] - e1[1] * e2[0]};
                const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                for (float &x : n)
                    x = len > 0 ? x / len : 0;
                for (const auto *p : {&a, &b, &c})
                    group().vertices.insert(group().vertices.end(), {(*p)[0], (*p)[1], (*p)[2], n[0], n[1], n[2]});
            }
        }
    }
    std::erase_if(groups, [](const ObjGroup &g) { return g.vertices.empty(); });
    if (groups.empty())
        throw InterchangeError("OBJ has no faces");
    return groups;
}

double unit_scale(std::string_view unit) {
    static const std::map<std::string_view, double> scale{
        {"m", 1.0}, {"cm", 0.01}, {"mm", 0.001}, {"in", 0.0254}, {"ft", 0.3048}};
    const auto it = scale.find(unit);
    return it == scale.end() ? 1.0 : it->second;
}

} // namespace sv
