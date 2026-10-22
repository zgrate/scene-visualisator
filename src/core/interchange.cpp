#include "core/interchange.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

std::vector<std::vector<std::string>> parse_csv(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF"))
        text.remove_prefix(3);
    const std::string_view head = text.substr(0, text.find('\n'));
    char sep = ',';
    for (char c : {';', '\t'})
        if (std::count(head.begin(), head.end(), c) > std::count(head.begin(), head.end(), sep))
            sep = c;
    std::vector<std::vector<std::string>> rows(1);
    std::string cell;
    bool quoted = false, any = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"' && i + 1 < text.size() && text[i + 1] == '"')
                cell += text[++i];
            else if (c == '"')
                quoted = false;
            else
                cell += c;
        } else if (c == '"') {
            quoted = any = true;
        } else if (c == sep) {
            rows.back().push_back(std::move(cell));
            cell.clear();
            any = true;
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
            if (any || !cell.empty())
                rows.back().push_back(std::move(cell));
            cell.clear();
            any = false;
            if (!rows.back().empty())
                rows.emplace_back();
        } else {
            cell += c;
            any = true;
        }
    }
    if (any || !cell.empty())
        rows.back().push_back(std::move(cell));
    if (rows.back().empty())
        rows.pop_back();
    return rows;
}

CsvMapping guess_csv_mapping(const std::vector<std::string> &header) {
    static const std::map<std::string, std::vector<std::string>> synonyms{
        {"fixtureId", {"fixtureid", "fid", "id", "channel", "unitnumber", "fixturenumber"}},
        {"name", {"name", "label", "fixturename"}},
        {"type", {"fixturetype", "type", "gdtf", "gdtfspec", "model", "fixturemodel", "instrumenttype"}},
        {"mode", {"mode", "dmxmode", "gdtfmode", "personality"}},
        {"universe", {"universe", "univ", "dmxuniverse"}},
        {"address", {"address", "dmxaddress", "addr", "startaddress", "absoluteaddress"}},
        {"patch", {"patch", "dmxpatch", "universeaddress"}},
        {"x", {"x", "posx", "positionx", "xlocation", "xpos"}},
        {"y", {"y", "posy", "positiony", "ylocation", "ypos"}},
        {"z", {"z", "posz", "positionz", "zlocation", "zpos"}},
        {"rotX", {"rotx", "rotationx", "xrotation"}},
        {"rotY", {"roty", "rotationy", "yrotation"}},
        {"rotZ", {"rotz", "rotationz", "zrotation"}}};
    std::vector<std::string> keys;
    for (const std::string &h : header) {
        std::string k;
        for (char c : h)
            if (std::isalnum(static_cast<unsigned char>(c)))
                k += char(std::tolower(static_cast<unsigned char>(c)));
        keys.push_back(std::move(k));
    }
    CsvMapping m;
    for (const auto &[field, names] : synonyms) // earlier synonyms win, so "FixtureType" beats a later "Type"
        for (const std::string &n : names)
            if (const auto it = std::find(keys.begin(), keys.end(), n); it != keys.end() && !m.contains(field)) {
                m[field] = int(it - keys.begin());
                break;
            }
    return m;
}

json csv_import(const json &project, const std::vector<std::vector<std::string>> &rows, const CsvMapping &mapping,
                const CsvImportOptions &options) {
    const auto cell = [&](const std::vector<std::string> &row, const char *field) -> std::optional<std::string> {
        const auto it = mapping.find(field);
        if (it == mapping.end() || it->second < 0 || std::size_t(it->second) >= row.size())
            return std::nullopt;
        std::string v = row[std::size_t(it->second)];
        v.erase(0, v.find_first_not_of(" \t"));
        v.erase(v.find_last_not_of(" \t") + 1);
        return v.empty() ? std::nullopt : std::optional(v);
    };
    const auto number = [](const std::optional<std::string> &s) -> std::optional<double> {
        if (!s)
            return std::nullopt;
        std::string t = *s;
        std::replace(t.begin(), t.end(), ',', '.'); // decimal comma from a ';' file
        char *end = nullptr;
        const double v = std::strtod(t.c_str(), &end);
        return end != t.c_str() && std::isfinite(v) ? std::optional(v) : std::nullopt;
    };
    std::map<int, int> by_id; // fixtureId -> object index
    const json &objects = project.at("objects");
    for (std::size_t i = 0; i < objects.size(); ++i)
        if (objects[i].value("type", "") == "fixture" && objects[i].contains("fixtureId") &&
            objects[i]["fixtureId"].is_number_integer())
            by_id[objects[i]["fixtureId"].get<int>()] = int(i);

    json patch = json::array();
    for (std::size_t r = 1; r < rows.size(); ++r) {
        const auto &row = rows[r];
        const auto id = number(cell(row, "fixtureId"));
        const auto name = cell(row, "name"), type = cell(row, "type"), mode = cell(row, "mode");
        if (!id && !name && !type)
            continue;
        json fields = json::object();
        if (id)
            fields["fixtureId"] = int(std::lround(*id));
        if (name)
            fields["name"] = *name;
        if (type || mode) {
            auto [gdtf, m] = options.resolve ? options.resolve(type.value_or(""), mode.value_or(""))
                                             : std::pair{type.value_or(""), mode.value_or("")};
            if (type)
                fields["gdtf"] = gdtf.empty() ? *type : gdtf;
            if (!m.empty())
                fields["mode"] = m;
        }
        // Patch: "u.a", or Universe + Address, or an absolute address alone.
        std::optional<double> u = number(cell(row, "universe")), a = number(cell(row, "address"));
        if (const auto p = cell(row, "patch"); p && p->find('.') != std::string::npos) {
            u = number(p->substr(0, p->find('.')));
            a = number(p->substr(p->find('.') + 1));
        }
        if (a && *a >= 1) {
            int uu = u ? int(*u) : 1, aa = int(*a);
            uu += (aa - 1) / 512;
            aa = (aa - 1) % 512 + 1;
            if (uu >= 1)
                fields["patch"] = {{"universe", uu}, {"address", aa}};
        }
        const auto vec = [&](const char *a0, const char *a1, const char *a2, double scale) -> std::optional<json> {
            const auto x = number(cell(row, a0)), y = number(cell(row, a1)), z = number(cell(row, a2));
            if (!x && !y && !z)
                return std::nullopt;
            const double v[3] = {x.value_or(0) * scale, y.value_or(0) * scale, z.value_or(0) * scale};
            // shortcut: Z-up rotations map axis by axis like positions, exact only for single-axis rotations.
            return options.z_up ? json{v[0], v[2], -v[1]} : json{v[0], v[1], v[2]};
        };
        if (const auto pos = vec("x", "y", "z", options.scale))
            fields["position"] = *pos;
        if (const auto rot = vec("rotX", "rotY", "rotZ", 1))
            fields["rotation"] = *rot;

        if (const auto it = id ? by_id.find(int(std::lround(*id))) : by_id.end(); it != by_id.end()) {
            const json &o = objects[std::size_t(it->second)];
            for (const auto &[k, v] : fields.items())
                patch.push_back({{"op", o.contains(k) ? "replace" : "add"},
                                 {"path", "/objects/" + std::to_string(it->second) + "/" + k},
                                 {"value", v}});
        } else {
            json o = new_fixture(name.value_or(type.value_or("Fixture")), "", "");
            o.update(fields);
            patch.push_back({{"op", "add"}, {"path", "/objects/-"}, {"value", std::move(o)}});
        }
    }
    return patch;
}

} // namespace sv
