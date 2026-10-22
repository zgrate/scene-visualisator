#include "core/mvr.h"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <numbers>
#include <pugixml.hpp>
#include <set>
#include <sstream>

namespace sv {

namespace {

using M3 = std::array<std::array<double, 3>, 3>; // [row][column]

// Our axes from MVR's: x stays, MVR z (up) is our y, MVR y (upstage) is our -z.
constexpr M3 to_ours{{{1, 0, 0}, {0, 0, 1}, {0, -1, 0}}};

M3 mul(const M3 &a, const M3 &b) {
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                r[i][j] += a[i][k] * b[k][j];
    return r;
}

M3 transposed(const M3 &a) {
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r[i][j] = a[j][i];
    return r;
}

constexpr double rad = std::numbers::pi / 180;

M3 euler(const std::array<double, 3> &r) { // Ry(r[1]) · Rx(r[0]) · Rz(r[2])
    const double cx = std::cos(r[0] * rad), sx = std::sin(r[0] * rad), cy = std::cos(r[1] * rad),
                 sy = std::sin(r[1] * rad), cz = std::cos(r[2] * rad), sz = std::sin(r[2] * rad);
    const M3 ry{{{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}}}, rx{{{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}}},
        rz{{{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}}};
    return mul(mul(ry, rx), rz);
}

// Rounded so that a value read, written and read again compares equal (lossless round trip).
double tidy(double v, double step) {
    const double r = std::round(v / step) * step;
    return r == 0 ? 0.0 : r; // no -0
}

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.12g", std::abs(v) < 1e-12 ? 0.0 : v);
    return buf;
}

std::array<double, 12> parse_matrix(const std::string &s) {
    std::array<double, 12> m{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    std::array<double, 12> read{};
    std::size_t n = 0;
    for (const char *p = s.c_str(); *p && n < 12;) {
        if (std::strchr("+-.0123456789", *p)) {
            char *end = nullptr;
            read[n++] = std::strtod(p, &end);
            p = end > p ? end : p + 1;
        } else {
            ++p;
        }
    }
    return n == 12 ? read : m;
}

std::string xml_of(pugi::xml_node n) {
    std::ostringstream out;
    n.print(out, "", pugi::format_raw);
    return out.str();
}

std::string lower_ext(const std::string &name) {
    const auto dot = name.rfind('.');
    std::string e = dot == std::string::npos ? "" : name.substr(dot + 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e;
}

std::string base_name(const std::string &path) { return path.substr(path.rfind('/') + 1); }

void add_box(std::vector<float> &v, float sx, float sy, float sz) {
    const float x = sx / 2, y = sy / 2, z = sz / 2;
    const float c[8][3] = {{-x, -y, -z}, {x, -y, -z}, {x, y, -z}, {-x, y, -z},
                           {-x, -y, z},  {x, -y, z},  {x, y, z},  {-x, y, z}};
    const int quads[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}};
    for (const auto &q : quads)
        for (int i : {q[0], q[1], q[2], q[0], q[2], q[3]})
            v.insert(v.end(), c[i], c[i] + 3);
}

// ---------------------------------------------------------------- import

struct Reader {
    MvrScene &out;
    const ZipEntries &zip;
    pugi::xml_node aux;
    std::set<std::string> gdtf_entries;

    const std::vector<std::uint8_t> *file(const std::string &name) const {
        const auto it = zip.find(name);
        return it == zip.end() ? nullptr : &it->second;
    }

    // First Geometry3D file of an object, through a Symbol's Symdef when needed.
    std::string geometry_file(pugi::xml_node n) const {
        const pugi::xml_node g = n.child("Geometries");
        if (pugi::xml_node g3 = g.find_node([](pugi::xml_node c) { return std::strcmp(c.name(), "Geometry3D") == 0; }))
            return g3.attribute("fileName").as_string();
        if (pugi::xml_node sym = g.child("Symbol")) {
            const std::string def = sym.attribute("symdef").as_string();
            for (pugi::xml_node s : aux.child("Symdefs").children("Symdef"))
                if (def == s.attribute("uuid").as_string())
                    if (pugi::xml_node g3 =
                            s.find_node([](pugi::xml_node c) { return std::strcmp(c.name(), "Geometry3D") == 0; }))
                        return g3.attribute("fileName").as_string();
        }
        return {};
    }

    void child_list(pugi::xml_node list, const json &layer, const json &parent) {
        for (pugi::xml_node c : list.children())
            if (c.type() == pugi::node_element)
                object(c, layer, parent);
    }

    void object(pugi::xml_node n, const json &layer, const json &parent) {
        const std::string el = n.name(), name = n.attribute("name").as_string();
        std::set<std::string> handled{"Matrix", "ChildList"};
        json o;
        if (el == "Fixture") {
            std::string spec = n.child_value("GDTFSpec");
            if (!spec.empty() && !file(spec) && file(spec + ".gdtf"))
                spec += ".gdtf";
            if (file(spec))
                gdtf_entries.insert(spec), out.files["gdtf/" + spec] = *file(spec);
            else
                out.warnings.push_back("\"" + name + "\": GDTF file \"" + spec + "\" is not in the MVR");
            o = new_fixture(name, spec, n.child_value("GDTFMode"));
            handled.insert({"GDTFSpec", "GDTFMode"});
            const std::string id = n.child_value("FixtureID");
            char *end = nullptr;
            const long v = std::strtol(id.c_str(), &end, 10);
            if (id.empty() || (*end == 0 && v > 0)) {
                o["fixtureId"] = id.empty() ? json() : json(v);
                handled.insert("FixtureID");
            }
            if (std::string_view(n.child_value("UnitNumber")) == "0")
                handled.insert("UnitNumber"); // the default, written back on export
            const pugi::xml_node addresses = n.child("Addresses");
            int count = 0;
            for (pugi::xml_node a : addresses.children("Address")) {
                ++count;
                if (a.attribute("break").as_int(0) != 0)
                    continue;
                const std::string text = a.child_value();
                int u = 1, ch = 0;
                if (const auto dot = text.find('.'); dot != std::string::npos) {
                    u = std::atoi(text.c_str()), ch = std::atoi(text.c_str() + dot + 1);
                } else if (const int abs = std::atoi(text.c_str()); abs > 0) {
                    u = (abs - 1) / 512 + 1, ch = (abs - 1) % 512 + 1;
                }
                if (u >= 1 && ch >= 1 && ch <= 512)
                    o["patch"] = {{"universe", u}, {"address", ch}};
            }
            if (count <= 1)
                handled.insert("Addresses"); // multi-break patches stay verbatim; break 0 drives the decode
        } else if (el == "GroupObject") {
            o = new_set_piece(name, "");
            o["type"] = "group";
            o.erase("geometry");
        } else {
            const std::string geo = geometry_file(n), ext = lower_ext(geo);
            const auto *bytes = file(geo);
            std::vector<ObjGroup> mesh;
            if (bytes && (ext == "glb" || ext == "gltf")) {
                try {
                    mesh = parse_gltf(*bytes);
                } catch (const InterchangeError &e) {
                    out.warnings.push_back("\"" + name + "\": " + geo + ": " + e.what());
                }
            } else if (!geo.empty() && !bytes) {
                out.warnings.push_back("\"" + name + "\": geometry \"" + geo + "\" is not in the MVR");
            }
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (const ObjGroup &g : mesh)
                for (std::size_t i = 0; i + 5 < g.vertices.size(); i += 6)
                    for (std::size_t k = 0; k < 3; ++k)
                        lo[k] = std::min(lo[k], g.vertices[i + k]), hi[k] = std::max(hi[k], g.vertices[i + k]);
            const float sx = hi[0] - lo[0], sy = hi[1] - lo[1], sz = hi[2] - lo[2];
            if (el == "VideoScreen") {
                o = new_led_wall(name);
                // shortcut: the wall is sized from its model's bounds but keeps our bottom-centre origin, so a
                // model not built that way draws offset; read the model's origin when real screens show it.
                if (sx > 0 && sy > 0)
                    o["width"] = tidy(sx, 1e-6), o["height"] = tidy(sy, 1e-6);
            } else if (el == "Truss" && (mesh.empty() || (std::max(sy, sz) <= 0.6f && sx >= 2 * std::max(sy, sz)))) {
                // A straight bar along X (or no readable model) becomes our built-in truss.
                // shortcut: the model is assumed centred on its origin; a truss modelled from one end draws offset.
                o = new_truss(name, "straight", mesh.empty() ? 3.0 : tidy(sx, 1e-6));
            } else {
                // shortcut: 3DS models are kept for the round trip but drawn as the missing-mesh marker box.
                o = new_set_piece(name, geo.empty() ? std::string() : "geometry/" + geo);
            }
        }
        o["uuid"] = n.attribute("uuid").empty() ? new_uuid() : std::string(n.attribute("uuid").as_string());
        o["parent"] = parent;
        o["layer"] = layer;
        const Transform t = transform_from_mvr(parse_matrix(n.child_value("Matrix")));
        o["position"] = t.position, o["rotation"] = t.rotation, o["scale"] = t.scale;

        json mvr = {{"element", el}, {"xml", ""}};
        std::string xml;
        for (pugi::xml_node c : n.children())
            if (c.type() != pugi::node_element || !handled.contains(c.name()))
                xml += xml_of(c);
        mvr["xml"] = xml;
        json attrs = json::object();
        for (pugi::xml_attribute a : n.attributes())
            if (std::string_view(a.name()) != "name" && std::string_view(a.name()) != "uuid")
                attrs[a.name()] = a.value();
        if (!attrs.empty())
            mvr["attributes"] = attrs;
        o["extras"] = {{"mvr", mvr}};
        const json uuid = o["uuid"];
        out.objects.push_back(std::move(o));
        child_list(n.child("ChildList"), layer, uuid);
    }
};

// ---------------------------------------------------------------- export

struct Writer {
    const ZipEntries &files;
    ZipEntries out;
    std::map<std::string, std::vector<const json *>> children; // parent uuid -> objects

    void child_list(pugi::xml_node parent, const std::vector<const json *> &objects) {
        if (objects.empty())
            return;
        pugi::xml_node l = parent.append_child("ChildList");
        for (const json *o : objects)
            write(l, *o);
    }

    static void text(pugi::xml_node n, const char *name, const std::string &value) {
        n.append_child(name).text().set(value.c_str());
    }

    // Geometry of an object without MVR geometry: a referenced or generated model file name, or empty.
    std::string geometry(const json &o) {
        const std::string type = o.value("type", std::string()), uuid = o.value("uuid", std::string());
        std::vector<float> v;
        if (type == "truss") {
            add_box(v, o.value("length", 3.0f), 0.29f, 0.29f);
        } else if (type == "ledWall") {
            const float w = o.value("width", 4.0f) / 2, h = o.value("height", 2.5f);
            v = {-w, 0, 0, w, 0, 0, w, h, 0, -w, 0, 0, w, h, 0, -w, h, 0};
        } else if (type == "setPiece") {
            const std::string geo = o.value("geometry", std::string());
            const auto it = files.find(geo);
            if (it == files.end())
                return {};
            if (lower_ext(geo) != "obj") {
                out[base_name(geo)] = it->second;
                return base_name(geo);
            }
            const std::string group = o.value("group", std::string());
            for (const ObjGroup &g : parse_obj({reinterpret_cast<const char *>(it->second.data()), it->second.size()}))
                if (group.empty() || g.name == group)
                    for (std::size_t i = 0; i + 5 < g.vertices.size(); i += 6)
                        v.insert(v.end(), g.vertices.begin() + long(i), g.vertices.begin() + long(i) + 3);
        }
        if (v.empty())
            return {};
        out[uuid + ".glb"] = write_glb(v);
        return uuid + ".glb";
    }

    void write(pugi::xml_node into, const json &o) {
        static const std::map<std::string, std::string> elements{{"fixture", "Fixture"},
                                                                 {"truss", "Truss"},
                                                                 {"ledWall", "VideoScreen"},
                                                                 {"setPiece", "SceneObject"},
                                                                 {"group", "GroupObject"}};
        const std::string type = o.value("type", std::string()), uuid = o.value("uuid", std::string());
        json mvr = json::object();
        if (o.contains("extras") && o["extras"].is_object() && o["extras"].contains("mvr"))
            mvr = o["extras"]["mvr"];
        std::string el = mvr.value("element", std::string());
        if (el.empty()) {
            const auto it = elements.find(type);
            if (it == elements.end())
                return; // pixel strips and cameras have no MVR object
            el = it->second;
        }
        pugi::xml_document kept; // the object's verbatim MVR children
        kept.load_string(mvr.value("xml", std::string()).c_str(), pugi::parse_default | pugi::parse_fragment);

        pugi::xml_node n = into.append_child(el.c_str());
        n.append_attribute("name").set_value(o.value("name", std::string()).c_str());
        n.append_attribute("uuid").set_value(uuid.c_str());
        if (mvr.contains("attributes"))
            for (const auto &[k, v] : mvr["attributes"].items())
                if (v.is_string())
                    n.append_attribute(k.c_str()).set_value(v.get<std::string>().c_str());

        Transform t;
        const auto vec = [&](const char *key, std::array<double, 3> &dst) {
            if (o.contains(key) && o[key].is_array() && o[key].size() == 3)
                for (std::size_t i = 0; i < 3; ++i)
                    dst[i] = o[key][i].get<double>();
        };
        vec("position", t.position), vec("rotation", t.rotation), vec("scale", t.scale);
        const std::array<double, 12> m = mvr_from_transform(t);
        std::string mt;
        for (int c = 0; c < 4; ++c)
            mt += "{" + num(m[c * 3]) + "," + num(m[c * 3 + 1]) + "," + num(m[c * 3 + 2]) + "}";
        text(n, "Matrix", mt);

        if (type == "fixture") {
            const std::string gdtf = o.value("gdtf", std::string());
            text(n, "GDTFSpec", gdtf);
            text(n, "GDTFMode", o.value("mode", std::string()));
            if (const auto it = files.find("gdtf/" + gdtf); it != files.end())
                out[gdtf] = it->second;
            if (!kept.child("FixtureID"))
                text(n, "FixtureID",
                     o.value("fixtureId", json()).is_number() ? std::to_string(o["fixtureId"].get<int>())
                                                              : std::string());
            if (!kept.child("UnitNumber"))
                text(n, "UnitNumber", "0");
            const json &p = o.value("patch", json());
            if (!kept.child("Addresses") && p.is_object()) {
                pugi::xml_node a = n.append_child("Addresses").append_child("Address");
                a.append_attribute("break").set_value(0);
                a.text().set((p.value("universe", 1) - 1) * 512 + p.value("address", 1));
            }
        } else if (type != "group" && !kept.child("Geometries")) {
            if (const std::string file = geometry(o); !file.empty())
                n.append_child("Geometries")
                    .append_child("Geometry3D")
                    .append_attribute("fileName")
                    .set_value(file.c_str());
        }
        for (pugi::xml_node c : kept.children())
            n.append_copy(c);
        child_list(n, children[uuid]);
    }
};

} // namespace

Transform transform_from_matrix(const std::array<double, 12> &m) {
    M3 l{};
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            l[r][c] = m[std::size_t(c * 3 + r)];
    Transform t;
    for (int c = 0; c < 3; ++c) {
        double s = std::hypot(l[0][c], l[1][c], l[2][c]);
        s = s < 1e-12 ? 1 : s;
        t.scale[std::size_t(c)] = tidy(s, 1e-9);
        for (int r = 0; r < 3; ++r)
            l[r][c] /= s;
    }
    // shortcut: a mirrored matrix (negative determinant) loses its mirror; no rig file has shown one yet.
    double rx = std::asin(std::clamp(-l[1][2], -1.0, 1.0)), ry, rz;
    if (std::abs(std::cos(rx)) > 1e-6)
        ry = std::atan2(l[0][2], l[2][2]), rz = std::atan2(l[1][0], l[1][1]);
    else
        ry = std::atan2(-l[2][0], l[0][0]), rz = 0; // gimbal lock: fold Z into Y
    // Every rotation has a twin (180 - x, y + 180, z + 180); keep the one with the smaller angles, so a hung
    // fixture reads (180, 0, 0) and not (0, 180, 180).
    const auto wrap = [](double d) { return tidy(d - 360 * std::ceil((d - 180) / 360), 1e-6); };
    std::array<double, 3> ra{wrap(rx / rad), wrap(ry / rad), wrap(rz / rad)},
        rb{wrap(180 - rx / rad), wrap(ry / rad + 180), wrap(rz / rad + 180)};
    const auto cost = [](const std::array<double, 3> &r) { return std::abs(r[0]) + std::abs(r[1]) + std::abs(r[2]); };
    t.rotation = cost(rb) < cost(ra) - 1e-6 ? rb : ra;
    for (int r = 0; r < 3; ++r)
        t.position[std::size_t(r)] = tidy(m[std::size_t(9 + r)], 1e-6);
    return t;
}

Transform transform_from_mvr(const std::array<double, 12> &m) {
    M3 a{};
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            a[r][c] = m[std::size_t(c * 3 + r)];
    const M3 l = mul(mul(to_ours, a), transposed(to_ours));
    std::array<double, 12> ours{};
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            ours[std::size_t(c * 3 + r)] = l[r][c];
    for (int r = 0; r < 3; ++r)
        ours[std::size_t(9 + r)] = (to_ours[r][0] * m[9] + to_ours[r][1] * m[10] + to_ours[r][2] * m[11]) / 1000;
    return transform_from_matrix(ours);
}

std::array<double, 12> mvr_from_transform(const Transform &t) {
    M3 l = euler(t.rotation);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            l[r][c] *= t.scale[std::size_t(c)];
    const M3 a = mul(mul(transposed(to_ours), l), to_ours);
    std::array<double, 12> m{};
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            m[std::size_t(c * 3 + r)] = a[r][c];
    for (int r = 0; r < 3; ++r) // MVR = transpose(to_ours) · ours, in mm
        m[std::size_t(9 + r)] =
            (to_ours[0][r] * t.position[0] + to_ours[1][r] * t.position[1] + to_ours[2][r] * t.position[2]) * 1000;
    return m;
}

std::vector<ObjGroup> parse_gltf(std::span<const std::uint8_t> bytes) {
    cgltf_options options{};
    cgltf_data *raw = nullptr;
    if (cgltf_parse(&options, bytes.data(), bytes.size(), &raw) != cgltf_result_success)
        throw InterchangeError("not a glTF file");
    const std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(raw, cgltf_free);
    if (cgltf_load_buffers(&options, data.get(), nullptr) != cgltf_result_success)
        throw InterchangeError("glTF buffers must be embedded (GLB or data URIs)");
    std::vector<ObjGroup> groups;
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const cgltf_node &node = data->nodes[i];
        if (!node.mesh)
            continue;
        float w[16]; // column major
        cgltf_node_transform_world(&node, w);
        ObjGroup g{node.name ? node.name : node.mesh->name ? node.mesh->name : "mesh" + std::to_string(i), {}};
        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
            const cgltf_primitive &prim = node.mesh->primitives[p];
            const cgltf_accessor *pos = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a)
                if (prim.attributes[a].type == cgltf_attribute_type_position)
                    pos = prim.attributes[a].data;
            if (prim.type != cgltf_primitive_type_triangles || !pos)
                continue;
            const cgltf_size count = prim.indices ? prim.indices->count : pos->count;
            for (cgltf_size k = 0; k + 2 < count; k += 3) {
                float v[3][3];
                for (int j = 0; j < 3; ++j) {
                    const cgltf_size idx =
                        prim.indices ? cgltf_accessor_read_index(prim.indices, k + cgltf_size(j)) : k + cgltf_size(j);
                    float in[3] = {};
                    cgltf_accessor_read_float(pos, idx, in, 3);
                    for (int r = 0; r < 3; ++r)
                        v[j][r] = w[r] * in[0] + w[4 + r] * in[1] + w[8 + r] * in[2] + w[12 + r];
                }
                const float e1[3] = {v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2]},
                            e2[3] = {v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2]};
                float nrm[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                e1[0] * e2[1] - e1[1] * e2[0]};
                const float len = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
                for (float &c : nrm)
                    c = len > 0 ? c / len : 0;
                for (const auto &vert : v) {
                    g.vertices.insert(g.vertices.end(), vert, vert + 3);
                    g.vertices.insert(g.vertices.end(), nrm, nrm + 3);
                }
            }
        }
        if (!g.vertices.empty())
            groups.push_back(std::move(g));
    }
    if (groups.empty())
        throw InterchangeError("glTF file has no triangles");
    return groups;
}

std::vector<std::uint8_t> write_glb(const std::vector<float> &positions) {
    const std::size_t count = positions.size() / 3, bin_size = count * 12;
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    for (std::size_t i = 0; i < count; ++i)
        for (std::size_t k = 0; k < 3; ++k) {
            const float c = positions[i * 3 + k];
            lo[k] = i ? std::min(lo[k], c) : c, hi[k] = i ? std::max(hi[k], c) : c;
        }
    const json gltf = {
        {"asset", {{"version", "2.0"}, {"generator", "Scene Visualiser"}}},
        {"scene", 0},
        {"scenes", {{{"nodes", {0}}}}},
        {"nodes", {{{"mesh", 0}}}},
        {"meshes", {{{"primitives", {{{"attributes", {{"POSITION", 0}}}}}}}}},
        {"accessors",
         {{{"bufferView", 0},
           {"componentType", 5126},
           {"count", count},
           {"type", "VEC3"},
           {"min", {lo[0], lo[1], lo[2]}},
           {"max", {hi[0], hi[1], hi[2]}}}}},
        {"bufferViews", {{{"buffer", 0}, {"byteLength", bin_size}}}},
        {"buffers", {{{"byteLength", bin_size}}}},
    };
    std::string js = gltf.dump();
    js.resize((js.size() + 3) / 4 * 4, ' ');
    const std::size_t bin_padded = (bin_size + 3) / 4 * 4;
    std::vector<std::uint8_t> out;
    // shortcut: words are written in host order; every platform we build for is little endian, as GLB requires.
    const auto u32 = [&](std::uint32_t v) {
        const auto *p = reinterpret_cast<const std::uint8_t *>(&v);
        out.insert(out.end(), p, p + 4);
    };
    u32(0x46546C67), u32(2), u32(std::uint32_t(12 + 8 + js.size() + 8 + bin_padded));
    u32(std::uint32_t(js.size())), u32(0x4E4F534A);
    out.insert(out.end(), js.begin(), js.end());
    u32(std::uint32_t(bin_padded)), u32(0x004E4942);
    const auto *p = reinterpret_cast<const std::uint8_t *>(positions.data());
    out.insert(out.end(), p, p + bin_size);
    out.resize(out.size() + bin_padded - bin_size, 0);
    return out;
}

MvrScene mvr_import(std::span<const std::uint8_t> bytes) {
    ZipEntries zip;
    try {
        zip = read_zip(bytes);
    } catch (const ProjectFileError &e) {
        throw InterchangeError(std::string("not an MVR file: ") + e.what());
    }
    const auto it = zip.find("GeneralSceneDescription.xml");
    if (it == zip.end())
        throw InterchangeError("not an MVR file: GeneralSceneDescription.xml missing");
    pugi::xml_document doc;
    if (!doc.load_buffer(it->second.data(), it->second.size()))
        throw InterchangeError("GeneralSceneDescription.xml is not valid XML");
    const pugi::xml_node root = doc.child("GeneralSceneDescription"), scene = root.child("Scene");
    if (!root)
        throw InterchangeError("not an MVR scene description");

    MvrScene out;
    Reader reader{out, zip, scene.child("AUXData"), {}};
    if (reader.aux)
        out.aux_data = xml_of(reader.aux);
    for (pugi::xml_node l : scene.child("Layers").children("Layer")) {
        const std::string uuid = l.attribute("uuid").empty() ? new_uuid() : l.attribute("uuid").as_string();
        json layer = {{"uuid", uuid}, {"name", l.attribute("name").as_string()}, {"visible", true}, {"locked", false}};
        std::string xml;
        for (pugi::xml_node c : l.children())
            if (std::string_view(c.name()) != "ChildList")
                xml += xml_of(c);
        if (!xml.empty())
            layer["extras"] = {{"mvr", {{"xml", xml}}}};
        out.layers.push_back(std::move(layer));
        reader.child_list(l.child("ChildList"), uuid, nullptr);
    }
    for (const auto &[name, data] : zip)
        if (name != "GeneralSceneDescription.xml" && !reader.gdtf_entries.contains(name) && lower_ext(name) != "gdtf")
            out.files["geometry/" + name] = data;
    return out;
}

std::vector<std::uint8_t> mvr_export(const json &project, const ZipEntries &files) {
    Writer w{files, {}, {}};
    const json &objects = project.value("objects", json::array());
    std::set<std::string> uuids, layer_ids;
    for (const json &o : objects)
        uuids.insert(o.value("uuid", std::string()));
    json layers = project.value("layers", json::array());
    for (const json &l : layers)
        layer_ids.insert(l.value("uuid", std::string()));
    std::map<std::string, std::vector<const json *>> roots; // layer uuid -> top-level objects
    std::string fallback;
    for (const json &o : objects) {
        const json &parent = o.value("parent", json());
        if (parent.is_string() && uuids.contains(parent.get<std::string>())) {
            w.children[parent.get<std::string>()].push_back(&o);
            continue;
        }
        std::string layer = o.value("layer", json()).is_string() ? o["layer"].get<std::string>() : std::string();
        if (!layer_ids.contains(layer)) {
            if (fallback.empty()) {
                fallback = new_uuid();
                layers.push_back({{"uuid", fallback}, {"name", "Scene Visualiser"}});
            }
            layer = fallback;
        }
        roots[layer].push_back(&o);
    }

    pugi::xml_document doc;
    pugi::xml_node decl = doc.append_child(pugi::node_declaration);
    decl.append_attribute("version").set_value("1.0");
    decl.append_attribute("encoding").set_value("UTF-8");
    pugi::xml_node root = doc.append_child("GeneralSceneDescription");
    root.append_attribute("verMajor").set_value(1);
    root.append_attribute("verMinor").set_value(6);
    root.append_attribute("provider").set_value("Scene Visualiser");
    root.append_attribute("providerVersion")
        .set_value(project.value("app", json::object()).value("version", "0.2.0").c_str());
    root.append_child("UserData");
    pugi::xml_node scene = root.append_child("Scene");
    pugi::xml_document aux;
    const json &mvr = project.value("mvr", json::object());
    if (aux.load_string(mvr.value("auxData", std::string()).c_str()) && aux.child("AUXData"))
        scene.append_copy(aux.child("AUXData"));
    else
        scene.append_child("AUXData");
    pugi::xml_node layer_list = scene.append_child("Layers");
    for (const json &l : layers) {
        pugi::xml_node n = layer_list.append_child("Layer");
        n.append_attribute("name").set_value(l.value("name", std::string()).c_str());
        n.append_attribute("uuid").set_value(l.value("uuid", std::string()).c_str());
        if (l.contains("extras") && l["extras"].contains("mvr")) {
            pugi::xml_document kept;
            kept.load_string(l["extras"]["mvr"].value("xml", std::string()).c_str(),
                             pugi::parse_default | pugi::parse_fragment);
            for (pugi::xml_node c : kept.children())
                n.append_copy(c);
        }
        pugi::xml_node list = n.append_child("ChildList");
        for (const json *o : roots[l.value("uuid", std::string())])
            w.write(list, *o);
    }

    // Models referenced by kept MVR geometry travel along.
    for (const auto &[name, data] : files)
        if (name.starts_with("geometry/") && lower_ext(name) != "obj")
            w.out.try_emplace(base_name(name), data);
    std::ostringstream xml;
    doc.save(xml, "  ", pugi::format_default, pugi::encoding_utf8);
    const std::string s = xml.str();
    w.out["GeneralSceneDescription.xml"].assign(s.begin(), s.end());
    return write_zip(w.out);
}

} // namespace sv
