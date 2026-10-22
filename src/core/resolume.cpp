#include "core/resolume.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <numbers>
#include <pugixml.hpp>
#include <set>
#include <sstream>

namespace sv {
namespace {

#include "core/resolume_templates.inc"

constexpr double deg = 180.0 / std::numbers::pi;

std::string g20(double v) {
    char s[40];
    std::snprintf(s, sizeof s, "%.20g", v);
    return s;
}
// Arena writes geometry as float32 printed with 20 significant digits.
std::string f20(double v) { return g20(double(float(v))); }

// Params our own fields carry; resolume.params never holds them.
const std::set<std::string, std::less<>> modelled{"Params/Name",
                                                  "Common/Name",
                                                  "Input/Start Channel",
                                                  "FixtureInstance/Fixture/Params/Pixels/Width",
                                                  "FixtureInstance/Fixture/Params/Pixels/Height",
                                                  "FixtureInstance/Fixture/Params/Pixels/Color Format",
                                                  "FixtureInstance/Fixture/Params/Pixels/Distribution",
                                                  "FixtureInstance/Fixture/Params/Pixels/Gamma"};
constexpr const char *pixels_path = "FixtureInstance/Fixture/Params/Pixels/";

// One path segment, escaped like a JSON pointer token: a Params or Param* element by its name, anything else by its tag, plus "#type" when typed.
std::string segment(pugi::xml_node n) {
    const std::string_view tag = n.name();
    std::string s;
    for (const char ch : tag == "Params" || tag.starts_with("Param") ? std::string_view(n.attribute("name").value()) : tag)
        s += ch == '~' ? "~0" : ch == '/' ? "~1" : std::string(1, ch); // JSON-pointer escapes: "Input Bypass/Solo"
    if (const auto t = n.attribute("type"))
        s += std::string("#") + t.value();
    return s;
}

bool is_param(pugi::xml_node n) { return std::string_view(n.name()).starts_with("Param") && n.attribute("value"); }

// Every param below `root` (child slices under <layers> excluded), in document order.
void each_param(pugi::xml_node node, const std::string &prefix, const auto &fn) {
    for (pugi::xml_node c : node.children()) {
        if (c.type() != pugi::node_element || std::string_view(c.name()) == "layers" ||
            std::string_view(c.name()) == "screens")
            continue;
        const std::string key = prefix + segment(c);
        if (is_param(c))
            fn(key, c);
        else
            each_param(c, key + "/", fn);
    }
}

pugi::xml_node find_param(pugi::xml_node root, std::string_view key) {
    pugi::xml_node n = root;
    while (n && !key.empty()) {
        const auto slash = key.find('/');
        const std::string_view seg = key.substr(0, slash);
        pugi::xml_node next;
        for (pugi::xml_node c : n.children())
            if (c.type() == pugi::node_element && segment(c) == seg) {
                next = c;
                break;
            }
        n = next;
        key = slash == std::string_view::npos ? std::string_view() : key.substr(slash + 1);
    }
    return n;
}

std::optional<std::pair<double, double>> min_max(pugi::xml_node param) {
    if (const pugi::xml_node r = param.find_child_by_attribute("ValueRange", "name", "minMax"))
        return std::pair(r.attribute("min").as_double(), r.attribute("max").as_double());
    return std::nullopt;
}

void set_attr(pugi::xml_node n, const char *name, const std::string &text) {
    if (std::string_view(n.attribute(name).value()) != text)
        (n.attribute(name) ? n.attribute(name) : n.append_attribute(name)).set_value(text.c_str());
}

// Writes a param's value text and keeps Arena's normalised phase in step. Unknown keys are ignored.
void set_param(pugi::xml_node root, std::string_view key, const std::string &text) {
    if (key.starts_with('@')) {
        set_attr(root, std::string(key.substr(1)).c_str(), text);
        return;
    }
    const pugi::xml_node p = find_param(root, key);
    if (!p || std::string_view(p.attribute("value").value()) == text)
        return;
    p.attribute("value").set_value(text.c_str());
    const auto mm = min_max(p);
    if (const pugi::xml_node phase = p.child("PhaseSourceStatic"); phase && mm && mm->second > mm->first)
        phase.attribute("phase").set_value(g20((std::stod(text) - mm->first) / (mm->second - mm->first)).c_str());
}

// Numeric fields are compared by value, so a float32 text Arena wrote is not re-printed when nothing changed.
void set_number(pugi::xml_node root, std::string_view key, double v) {
    const pugi::xml_node p = find_param(root, key);
    if (p && p.attribute("value").as_double() != v)
        set_param(root, key, g20(v));
}

json params_of(pugi::xml_node el) {
    json params = json::object();
    each_param(el, "", [&](const std::string &key, pugi::xml_node p) {
        if (!modelled.contains(key))
            params[key] = p.attribute("value").as_string();
    });
    if (const auto id = el.attribute("LumiverseId"))
        params["@LumiverseId"] = id.value();
    return params;
}

std::string param_text(pugi::xml_node el, std::string_view key) {
    return find_param(el, key).attribute("value").as_string();
}

// A quad as an unrotated rect (composition pixels, top-left origin) plus angle in degrees, y-down like Arena.
json rect_of(pugi::xml_node quad) {
    double x[4] = {}, y[4] = {};
    int i = 0;
    for (pugi::xml_node v : quad.children("v"))
        if (i < 4)
            x[i] = v.attribute("x").as_double(), y[i] = v.attribute("y").as_double(), ++i;
    const double cx = (x[0] + x[1] + x[2] + x[3]) / 4, cy = (y[0] + y[1] + y[2] + y[3]) / 4;
    const double w = std::hypot(x[1] - x[0], y[1] - y[0]), h = std::hypot(x[3] - x[0], y[3] - y[0]);
    return {{"x", cx - w / 2},
            {"y", cy - h / 2},
            {"w", w},
            {"h", h},
            {"angle", std::atan2(y[1] - y[0], x[1] - x[0]) * deg}};
}

bool same_rect(const json &a, const json &b) {
    for (const char *k : {"x", "y", "w", "h", "angle"})
        if (std::abs(a.value(k, 0.0) - b.value(k, 0.0)) > 1e-3)
            return false;
    return true;
}

// Rewrites a quad only when the stored rect moved, so untouched quads keep Arena's exact text.
void write_quad(pugi::xml_node quad, const json &r) {
    if (!quad || !r.is_object() || same_rect(rect_of(quad), r))
        return;
    const double w = r.value("w", 0.0), h = r.value("h", 0.0), a = r.value("angle", 0.0) / deg;
    const double cx = r.value("x", 0.0) + w / 2, cy = r.value("y", 0.0) + h / 2;
    const double c = std::cos(a), s = std::sin(a);
    const double corners[4][2] = {{-w / 2, -h / 2}, {w / 2, -h / 2}, {w / 2, h / 2}, {-w / 2, h / 2}};
    set_attr(quad, "orientation", a == 0 ? "0" : f20(a));
    while (quad.child("v"))
        quad.remove_child("v");
    for (const auto &p : corners) {
        pugi::xml_node v = quad.append_child("v");
        v.append_attribute("x").set_value(f20(cx + p[0] * c - p[1] * s).c_str());
        v.append_attribute("y").set_value(f20(cy + p[0] * s + p[1] * c).c_str());
    }
}

std::string uid_of(const json &o) {
    const json &r = o.value("resolume", json::object());
    return r.is_object() ? r.value("uniqueId", std::string()) : std::string();
}

const json *find(const json &list, const std::string &uid, const std::string &name, const char *type,
                 std::set<const json *> &taken) {
    for (int pass = 0; pass < 2; ++pass)
        for (const json &o : list)
            if ((!type || o.value("type", std::string()) == type) && !taken.contains(&o) &&
                (pass == 0 ? !uid.empty() && uid_of(o) == uid : o.value("name", std::string()) == name)) {
                taken.insert(&o);
                return &o;
            }
    return nullptr;
}

std::string device_kind(std::string_view tag) {
    std::string k(tag.starts_with("OutputDevice") ? tag.substr(12) : tag);
    std::transform(k.begin(), k.end(), k.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    return k;
}

int max_universe(const json &objects) {
    int u = 0;
    for (const json &o : objects)
        if (const json &p = o.value("patch", json()); p.is_object())
            u = std::max(u, p.value("universe", 0));
    return u;
}

} // namespace

std::string resolume_text(const json &v) {
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_boolean())
        return v.get<bool>() ? "1" : "0";
    if (v.is_number_integer())
        return std::to_string(v.get<long long>());
    if (v.is_number())
        return g20(v.get<double>());
    return {};
}

ResolumeImport resolume_import(const json &project, std::string_view xml, const ResolumeImportOptions &opt) {
    pugi::xml_document doc;
    if (const auto r = doc.load_buffer(xml.data(), xml.size()); !r)
        throw InterchangeError(std::string("XML parse error: ") + r.description());
    const pugi::xml_node setup = doc.child("XmlState").child("ScreenSetup");
    const pugi::xml_node screens = setup.child("screens");
    if (!screens)
        throw InterchangeError("no <ScreenSetup><screens>: not a Resolume Advanced Output file");

    json result = project;
    std::vector<std::string> warnings;
    json &comp = result["composition"];
    if (const pugi::xml_node size = setup.child("CurrentCompositionTextureSize"))
        for (const char *key : {"width", "height"})
            if (const int v = size.attribute(key).as_int(); v > 0)
                comp[key] = v;
    comp["resolume"] = {{"params", params_of(setup)}}; // SoftEdging and other global params
    const double comp_w = comp.value("width", 1920), comp_h = comp.value("height", 1080);
    const double pitch = opt.pixel_pitch / 1000; // metres per composition pixel

    const json old_outputs = project.value("outputs", json::array());
    const json old_objects = project.value("objects", json::array());
    std::set<const json *> taken;
    json outputs = json::array(), objects = json::array();
    std::set<const json *> replaced; // old objects the file now owns
    int next_universe = max_universe(old_objects) + 1;
    int row = 0;

    // Front-elevation placement: composition centre-bottom sits at (0, base, plane_z).
    const auto place = [&](json &o, const json &r, double lift) {
        const double w = r["w"], h = r["h"], a = r["angle"].get<double>() / deg;
        const double cx = r["x"].get<double>() + w / 2, cy = r["y"].get<double>() + h / 2;
        double x = (cx - comp_w / 2) * pitch, y = (comp_h - cy) * pitch + opt.base;
        x -= lift * std::sin(a), y -= lift * std::cos(a); // walls stand on their bottom edge
        if (!opt.place)
            x = -8 + 2.0 * row++, y = lift > 0 ? 0 : 0.5;
        o["position"] = {x, y, opt.plane_z};
        o["rotation"] = {0.0, 0.0, opt.place ? -r["angle"].get<double>() : 0.0};
    };

    std::map<std::string, std::vector<std::string>> lumiverse_targets; // Arena Net:Sub:Uni -> names, for warnings
    for (pugi::xml_node screen : screens.children()) {
        const std::string_view tag = screen.name();
        const bool dmx = tag == "DmxScreen";
        if (!dmx && tag != "Screen")
            continue;
        const std::string name = param_text(screen, "Params/Name").empty() ? screen.attribute("name").as_string()
                                                                           : param_text(screen, "Params/Name");
        const std::string uid = screen.attribute("uniqueId").as_string();
        const json *old = find(old_outputs, uid, name, nullptr, taken);
        json out = old ? *old : json{{"uuid", new_uuid()}};
        out["name"] = name;
        out["resolume"] = {{"uniqueId", uid}, {"params", params_of(screen)}};
        const pugi::xml_node device = screen.child("OutputDevice").first_child();
        out["kind"] = dmx ? "lumiverse" : device_kind(device.name());
        if (!dmx) {
            out["deviceName"] = device.attribute("name").as_string();
            out["width"] = device.attribute("width").as_int();
            out["height"] = device.attribute("height").as_int();
        }

        int span = 1; // universes this Lumiverse needs
        if (dmx) {
            const std::string dp = "OutputDevice/OutputDeviceDmx/DmxOutputParams/";
            lumiverse_targets[param_text(screen, dp + "TargetIP") + " " + param_text(screen, dp + "Subnet") + ":" +
                              param_text(screen, dp + "Universe")]
                .push_back(name);
            if (!out.contains("universe"))
                out["universe"] = next_universe;
        }
        for (pugi::xml_node slice : screen.child("layers").children()) {
            const bool strip = std::string_view(slice.name()) == "DmxSlice";
            if (!strip && std::string_view(slice.name()) != "Slice")
                continue;
            const std::string sname = param_text(slice, "Common/Name");
            const json *prev = find(old_objects, slice.attribute("uniqueId").as_string(), sname,
                                    strip ? "pixelStrip" : "ledWall", taken);
            const json r = rect_of(slice.child("InputRect"));
            json o = prev ? *prev : strip ? new_pixel_strip(sname) : new_led_wall(sname);
            if (prev)
                replaced.insert(prev);
            o["name"] = sname;
            o["output"] = out["uuid"];
            o["slice"] = r;
            o["resolume"] = {{"uniqueId", slice.attribute("uniqueId").as_string()}, {"params", params_of(slice)}};
            if (const std::string src = param_text(slice, "Input/Input Source"); src != "0:1")
                warnings.push_back("\"" + sname + "\" takes layer input " + src +
                                   "; the composition preview shows the full composition there");
            if (strip) {
                const int px =
                    int(find_param(slice, std::string(pixels_path) + "Width").attribute("value").as_double());
                o["pixels"] = px;
                o["rows"] = int(find_param(slice, std::string(pixels_path) + "Height").attribute("value").as_double());
                o["colorFormat"] = param_text(slice, std::string(pixels_path) + "Color Format");
                o["distribution"] =
                    find_param(slice, std::string(pixels_path) + "Distribution").attribute("value").as_int();
                o["gamma"] = find_param(slice, std::string(pixels_path) + "Gamma").attribute("value").as_double();
                const int start = int(find_param(slice, "Input/Start Channel").attribute("value").as_double());
                o["patch"] = {{"universe", out["universe"].get<int>() + (start - 1) / 512},
                              {"address", (start - 1) % 512 + 1}};
                const DmxAddress end = strip_pixel_address(o, px * o["rows"].get<int>() - 1);
                span = std::max(span, end.universe - out["universe"].get<int>() + 1);
                if (!prev) {
                    o["length"] = opt.place ? r["w"].get<double>() * pitch : px / 60.0;
                    place(o, r, 0);
                }
            } else {
                o["outRect"] = rect_of(slice.child("OutputRect"));
                if (!prev) {
                    o["width"] = r["w"].get<double>() * pitch;
                    o["height"] = r["h"].get<double>() * pitch;
                    o["pixelPitch"] = opt.pixel_pitch;
                    place(o, r, o["height"].get<double>() / 2);
                }
            }
            objects.push_back(std::move(o));
        }
        if (dmx && out["universe"].get<int>() >= next_universe)
            next_universe = out["universe"].get<int>() + span;
        outputs.push_back(std::move(out));
    }
    for (const auto &[target, names] : lumiverse_targets)
        if (names.size() > 1)
            warnings.push_back(std::to_string(names.size()) + " Lumiverses send to the same Art-Net target (" + target +
                               "); each got its own project universes, give them distinct universes in Arena to "
                               "preview them live");

    // Objects and outputs the file does not mention stay, in their old order, ahead of the imported ones.
    json kept = json::array();
    for (const json &o : old_objects)
        if (!replaced.contains(&o))
            kept.push_back(o);
    for (json &o : objects)
        kept.push_back(std::move(o));
    for (const json &o : old_outputs)
        if (!taken.contains(&o))
            outputs.push_back(o);
    result["objects"] = std::move(kept);
    result["outputs"] = std::move(outputs);
    result["resolumeSource"] = std::string(xml);
    return {json::diff(project, result), std::move(warnings)};
}

std::string resolume_export(const json &project) {
    const std::string source = project.value("resolumeSource", std::string());
    pugi::xml_document doc;
    std::string tail = "\r\n";
    bool crlf = true; // Arena is a Windows-first app
    if (!source.empty()) {
        if (!doc.load_buffer(source.data(), source.size(), pugi::parse_default | pugi::parse_declaration))
            throw InterchangeError("stored Resolume file does not parse");
        crlf = source.find("\r\n") != std::string::npos;
        tail = source.substr(source.find_last_not_of(" \t\r\n") + 1);
    } else {
        pugi::xml_node decl = doc.append_child(pugi::node_declaration);
        decl.append_attribute("version") = "1.0";
        decl.append_attribute("encoding") = "utf-8";
        pugi::xml_node state = doc.append_child("XmlState");
        state.append_attribute("name") = project.value("name", std::string("Scene Visualiser")).c_str();
        pugi::xml_node v = state.append_child("versionInfo");
        for (const auto &[k, val] : {std::pair{"name", "Resolume Arena"},
                                     {"majorVersion", "7"},
                                     {"minorVersion", "17"},
                                     {"microVersion", "3"},
                                     {"revision", "27437"}})
            v.append_attribute(k) = val;
        pugi::xml_node setup = state.append_child("ScreenSetup");
        setup.append_attribute("name") = "ScreenSetup";
        setup.append_child("Params").append_attribute("name") = "ScreenSetupParams";
        setup.append_child("CurrentCompositionTextureSize");
        setup.append_child("screens");
    }
    const pugi::xml_node setup = doc.child("XmlState").child("ScreenSetup");
    pugi::xml_node screens = setup.child("screens");
    if (!screens)
        throw InterchangeError("stored Resolume file has no <screens>");
    const json &comp = project.value("composition", json::object());
    const pugi::xml_node size = setup.child("CurrentCompositionTextureSize");
    set_attr(size, "width", std::to_string(comp.value("width", 1920)));
    set_attr(size, "height", std::to_string(comp.value("height", 1080)));

    if (const json &r = comp.value("resolume", json::object()); r.is_object() && r.contains("params"))
        for (const auto &[key, v] : r["params"].items())
            set_param(setup, key, resolume_text(v));

    std::map<std::string, pugi::xml_node> by_uid;
    long long next_uid = 1788000000000; // Arena's ids are millisecond timestamps
    for (pugi::xpath_node n : doc.select_nodes("//*[@uniqueId]")) {
        by_uid[n.node().attribute("uniqueId").value()] = n.node();
        next_uid = std::max(next_uid, n.node().attribute("uniqueId").as_llong() + 1);
    }
    std::set<pugi::xml_node> kept; // element roots written this export
    // New screens go after the last screen of their kind, so video Screens stay ahead of Lumiverses.
    const auto add_screen = [&](bool dmx) {
        pugi::xml_document t;
        t.load_string(dmx ? template_dmx_screen : template_screen);
        pugi::xml_node last;
        for (pugi::xml_node s : screens.children(dmx ? "DmxScreen" : "Screen"))
            last = s;
        const pugi::xml_node tpl = t.document_element();
        pugi::xml_node n = last ? screens.insert_copy_after(tpl, last)
                           : dmx ? screens.append_copy(tpl)
                                 : screens.prepend_copy(tpl);
        n.attribute("uniqueId").set_value(std::to_string(next_uid++).c_str());
        return n;
    };

    const auto write_output = [&](pugi::xml_node n, const json &out) {
        const std::string name = out.value("name", std::string());
        set_attr(n, "name", name);
        set_param(n, "Params/Name", name);
        if (std::string_view(n.name()) == "Screen") {
            // shortcut: the device keeps its kind (the template is NDI) and Arena's deviceId/idHash; changing an
            // output's kind is done in Arena until Spout/display templates are cut from a real file.
            pugi::xml_node device = n.child("OutputDevice").first_child();
            set_attr(device, "name", out.value("deviceName", name));
            set_attr(device, "width", std::to_string(out.value("width", 1920)));
            set_attr(device, "height", std::to_string(out.value("height", 1080)));
        }
        const json &r = out.value("resolume", json::object());
        if (r.is_object() && r.contains("params"))
            for (const auto &[key, v] : r["params"].items())
                set_param(n, key, resolume_text(v));
        kept.insert(n);
    };

    std::map<std::string, pugi::xml_node> screen_of; // output uuid -> element
    for (const json &out : project.value("outputs", json::array())) {
        const bool dmx = out.value("kind", std::string()) == "lumiverse";
        auto it = by_uid.find(uid_of(out));
        pugi::xml_node n = it != by_uid.end() && std::string_view(it->second.name()) == (dmx ? "DmxScreen" : "Screen")
                               ? it->second
                               : add_screen(dmx);
        write_output(n, out);
        screen_of[out.value("uuid", std::string())] = n;
    }

    pugi::xml_node shared_lumiverse; // strips without an output
    for (const json &o : project.value("objects", json::array())) {
        const std::string type = o.value("type", std::string());
        const bool strip = type == "pixelStrip";
        if (!strip && type != "ledWall")
            continue;
        const json out_uuid = o.value("output", json());
        pugi::xml_node screen;
        if (out_uuid.is_string() && screen_of.contains(out_uuid.get<std::string>()))
            screen = screen_of[out_uuid.get<std::string>()];
        if (screen && (std::string_view(screen.name()) == "DmxScreen") != strip)
            screen = {};
        if (!screen && strip) {
            if (!shared_lumiverse)
                write_output(shared_lumiverse = add_screen(true), {{"name", "Lumiverse"}});
            screen = shared_lumiverse;
        } else if (!screen) { // a wall of its own: one NDI screen at its native resolution
            const WallDerived d = wall_derived(o);
            screen = add_screen(false);
            write_output(screen, {{"name", o.value("name", std::string())},
                                  {"width", std::max(1, d.pixels_x)},
                                  {"height", std::max(1, d.pixels_y)}});
        }
        auto it = by_uid.find(uid_of(o));
        pugi::xml_node n;
        if (it != by_uid.end() && std::string_view(it->second.name()) == (strip ? "DmxSlice" : "Slice"))
            n = it->second;
        pugi::xml_node layers = screen.child("layers") ? screen.child("layers") : screen.append_child("layers");
        if (!n) {
            pugi::xml_document t;
            t.load_string(strip ? template_dmx_slice : template_slice);
            n = layers.append_copy(t.document_element());
            n.attribute("uniqueId").set_value(std::to_string(next_uid++).c_str());
        } else if (n.parent() != layers)
            n = layers.append_move(n);
        kept.insert(n);

        set_param(n, "Common/Name", o.value("name", std::string()));
        if (const json &r = o.value("resolume", json::object()); r.is_object() && r.contains("params"))
            for (const auto &[key, v] : r["params"].items())
                set_param(n, key, resolume_text(v));
        write_quad(n.child("InputRect"), o.value("slice", json()));
        if (strip) {
            const std::string px = pixels_path;
            set_number(n, px + "Width", o.value("pixels", 1));
            set_number(n, px + "Height", o.value("rows", 1));
            set_param(n, px + "Color Format", o.value("colorFormat", std::string("rgb")));
            set_number(n, px + "Distribution", o.value("distribution", 170));
            set_number(n, px + "Gamma", o.value("gamma", 2.5));
            const json &p = o.value("patch", json());
            if (p.is_object()) {
                // Start Channel counts from the Lumiverse's first universe.
                int base = 1;
                for (const json &out : project.value("outputs", json::array()))
                    if (out_uuid.is_string() && out.value("uuid", std::string()) == out_uuid.get<std::string>())
                        base = out.value("universe", 1);
                const int start = (p.value("universe", 1) - base) * 512 + p.value("address", 1);
                set_number(n, "Input/Start Channel", std::max(1, start));
            }
        } else {
            json out_rect = o.value("outRect", json());
            if (!out_rect.is_object()) { // a new slice fills its screen
                const WallDerived d = wall_derived(o);
                out_rect = {{"x", 0}, {"y", 0}, {"w", d.pixels_x}, {"h", d.pixels_y}, {"angle", 0}};
            }
            write_quad(n.child("OutputRect"), out_rect);
        }
    }

    // Whatever the project no longer has goes.
    std::vector<pugi::xml_node> gone;
    for (pugi::xml_node s : screens.children()) {
        const std::string_view tag = s.name();
        if (tag != "Screen" && tag != "DmxScreen")
            continue;
        if (!kept.contains(s)) {
            gone.push_back(s);
            continue;
        }
        for (pugi::xml_node sl : s.child("layers").children())
            if ((std::string_view(sl.name()) == "Slice" || std::string_view(sl.name()) == "DmxSlice") &&
                !kept.contains(sl))
                gone.push_back(sl);
    }
    for (pugi::xml_node n : gone)
        n.parent().remove_child(n);

    std::ostringstream out;
    doc.save(out, "\t", pugi::format_default, pugi::encoding_utf8);
    std::string text = out.str();
    std::string fixed;
    fixed.reserve(text.size() + text.size() / 16);
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text.compare(i, 3, " />") == 0) { // Arena writes "<a b=\"c\"/>"
            fixed += "/>";
            i += 2;
        } else if (text[i] == '\n' && crlf)
            fixed += "\r\n";
        else
            fixed += text[i];
    }
    while (!fixed.empty() && (fixed.back() == '\n' || fixed.back() == '\r'))
        fixed.pop_back();
    return fixed + tail;
}

std::vector<ResolumeParam> resolume_params(const json &project, const json &object) {
    const bool global = !object.contains("uuid");
    const bool output = !object.contains("type");
    const std::string type = object.value("type", std::string());
    const bool dmx = output ? object.value("kind", std::string()) == "lumiverse" : type == "pixelStrip";
    pugi::xml_document doc, t;
    pugi::xml_node el;
    const std::string source = project.value("resolumeSource", std::string());
    if (const std::string uid = uid_of(object);
        !uid.empty() && !source.empty() && doc.load_buffer(source.data(), source.size()))
        el = doc.find_node([&](pugi::xml_node n) { return uid == n.attribute("uniqueId").value(); });
    if (global) {
        if (!source.empty() && doc.load_buffer(source.data(), source.size()))
            el = doc.child("XmlState").child("ScreenSetup");
        if (!el)
            return {};
    } else if (!el) {
        t.load_string(output ? (dmx ? template_dmx_screen : template_screen)
                             : (dmx ? template_dmx_slice : template_slice));
        el = t.document_element();
    }
    const json &r = object.value("resolume", json::object());
    const json params = r.is_object() ? r.value("params", json::object()) : json::object();
    std::vector<ResolumeParam> list;
    if (const auto id = el.attribute("LumiverseId"))
        list.push_back({"@LumiverseId", "attribute", "INT32",
                        params.contains("@LumiverseId") ? resolume_text(params["@LumiverseId"]) : id.value(), "",
                        std::nullopt, std::nullopt});
    each_param(el, "", [&](const std::string &key, pugi::xml_node p) {
        if (modelled.contains(key))
            return;
        ResolumeParam rp{key,
                         p.name(),
                         p.attribute("T").as_string(),
                         params.contains(key) ? resolume_text(params[key]) : p.attribute("value").as_string(),
                         p.attribute("default").as_string(),
                         std::nullopt,
                         std::nullopt};
        if (const auto mm = min_max(p))
            rp.min = mm->first, rp.max = mm->second;
        list.push_back(std::move(rp));
    });
    return list;
}

} // namespace sv
