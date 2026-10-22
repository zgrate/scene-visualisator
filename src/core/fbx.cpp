#include "core/fbx.h"

#include <ufbx.h>

#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <numbers>
#include <set>

namespace sv {

namespace {

std::string str(ufbx_string s) { return {s.data, s.length}; }

struct Loader {
    FbxScene out;
    std::set<std::string> names;

    static bool has_mesh(const ufbx_node *n) {
        if (n->mesh)
            return true;
        for (const ufbx_node *c : n->children)
            if (has_mesh(c))
                return true;
        return false;
    }

    std::string unique(std::string name) {
        if (name.empty())
            name = "Mesh";
        std::string candidate = name;
        for (int i = 2; names.contains(candidate); ++i)
            candidate = name + " " + std::to_string(i);
        names.insert(candidate);
        return candidate;
    }

    void add_mesh(const ufbx_node *node, FbxNode &n) {
        const ufbx_mesh *mesh = node->mesh;
        ObjGroup g{unique(n.name), {}, {}};
        // shortcut: one colour per node (its first material, base colour only), no textures; split by
        // face_material when multi-material set pieces matter.
        const ufbx_material_list &mats = node->materials.count ? node->materials : mesh->materials;
        if (mats.count && mats.data[0]) {
            const ufbx_vec4 c = mats.data[0]->pbr.base_color.value_vec4;
            g.colour = {float(c.x), float(c.y), float(c.z), 1};
        }
        std::vector<uint32_t> tri(mesh->max_face_triangles * 3);
        for (const ufbx_face face : mesh->faces) {
            const uint32_t count = ufbx_triangulate_face(tri.data(), tri.size(), mesh, face);
            for (uint32_t t = 0; t < count; ++t) {
                ufbx_vec3 p[3];
                for (int k = 0; k < 3; ++k)
                    p[k] = ufbx_transform_position(&node->geometry_to_node,
                                                   ufbx_get_vertex_vec3(&mesh->vertex_position, tri[t * 3 + k]));
                const double e1[3] = {p[1].x - p[0].x, p[1].y - p[0].y, p[1].z - p[0].z},
                             e2[3] = {p[2].x - p[0].x, p[2].y - p[0].y, p[2].z - p[0].z};
                double nv[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                e1[0] * e2[1] - e1[1] * e2[0]};
                const double len = std::sqrt(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
                for (double &x : nv)
                    x = len > 0 ? x / len : 0;
                for (const ufbx_vec3 &v : p)
                    g.vertices.insert(g.vertices.end(),
                                      {float(v.x), float(v.y), float(v.z), float(nv[0]), float(nv[1]), float(nv[2])});
            }
        }
        if (g.vertices.empty())
            return;
        n.group = int(out.groups.size());
        out.groups.push_back(std::move(g));
    }

    void visit(const ufbx_node *node, int parent) {
        if (!has_mesh(node))
            return;
        FbxNode n;
        n.name = str(node->name);
        n.parent = parent;
        std::array<double, 12> m;
        std::memcpy(m.data(), node->node_to_parent.v, sizeof m); // same column layout (u, v, w, origin)
        n.transform = transform_from_matrix(m);
        if (node->mesh)
            add_mesh(node, n);
        const int index = int(out.nodes.size());
        out.nodes.push_back(std::move(n));
        for (const ufbx_node *c : node->children)
            visit(c, index);
    }
};

// Binary FBX: a record is end offset, property count, property bytes, name, properties, children, and a
// 13-byte null record after the children (also for a record without properties).
struct Node {
    std::string name;
    std::vector<std::string> props; // encoded, type code first
    std::vector<Node> kids;
};

template <typename T> void put(std::string &out, T v) { out.append(reinterpret_cast<const char *>(&v), sizeof v); }

std::string S(std::string_view s) {
    std::string p = "S";
    put(p, uint32_t(s.size()));
    return p.append(s);
}
std::string I(int32_t v) {
    std::string p = "I";
    put(p, v);
    return p;
}
std::string L(int64_t v) {
    std::string p = "L";
    put(p, v);
    return p;
}
std::string D(double v) {
    std::string p = "D";
    put(p, v);
    return p;
}
template <typename T> std::string array(char code, const std::vector<T> &v) {
    std::string p(1, code);
    put(p, uint32_t(v.size()));
    put(p, uint32_t(0)); // not compressed
    put(p, uint32_t(v.size() * sizeof(T)));
    p.append(reinterpret_cast<const char *>(v.data()), v.size() * sizeof(T));
    return p;
}

void write(std::string &out, const Node &n) {
    const std::size_t start = out.size();
    put(out, uint32_t(0));
    put(out, uint32_t(n.props.size()));
    std::size_t len = 0;
    for (const std::string &p : n.props)
        len += p.size();
    put(out, uint32_t(len));
    put(out, uint8_t(n.name.size()));
    out += n.name;
    for (const std::string &p : n.props)
        out += p;
    for (const Node &k : n.kids)
        write(out, k);
    if (!n.kids.empty() || n.props.empty())
        out.append(13, '\0');
    const uint32_t end = uint32_t(out.size());
    std::memcpy(out.data() + start, &end, 4);
}

// Properties70 "P" entries.
Node P(std::string_view name, std::string_view type, std::string_view label, std::string_view flags,
       std::vector<std::string> values) {
    Node n{"P", {S(name), S(type), S(label), S(flags)}, {}};
    n.props.insert(n.props.end(), values.begin(), values.end());
    return n;
}
Node P_int(std::string_view name, int v) { return P(name, "int", "Integer", "", {I(v)}); }
Node P_double(std::string_view name, double v) { return P(name, "double", "Number", "", {D(v)}); }
Node P_vec(std::string_view name, std::string_view type, std::array<double, 3> v) {
    return P(name, type, "", "A", {D(v[0]), D(v[1]), D(v[2])});
}
std::string object_name(std::string_view name, std::string_view cls) {
    return std::string(name) + std::string("\0\1", 2) + std::string(cls);
}

} // namespace

FbxScene load_fbx(std::span<const std::uint8_t> bytes) {
    ufbx_load_opts opts{};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1;
    opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    ufbx_error error;
    const std::unique_ptr<ufbx_scene, void (*)(ufbx_scene *)> scene(
        ufbx_load_memory(bytes.data(), bytes.size(), &opts, &error), ufbx_free_scene);
    if (!scene) {
        char text[512];
        ufbx_format_error(text, sizeof text, &error);
        throw InterchangeError(std::string("FBX: ") + text);
    }
    Loader loader;
    for (const ufbx_node *c : scene->root_node->children)
        loader.visit(c, -1);
    if (loader.out.groups.empty())
        throw InterchangeError("FBX has no meshes");
    return std::move(loader.out);
}

std::vector<std::uint8_t> write_fbx(const std::vector<FbxMesh> &meshes, const std::vector<FbxCamera> &cameras) {
    int64_t next_id = 1000;
    Node objects{"Objects", {}, {}}, connections{"Connections", {}, {}};
    const auto connect = [&](int64_t child, int64_t parent) {
        connections.kids.push_back({"C", {S("OO"), L(child), L(parent)}, {}});
    };
    int counts[4] = {}; // models, geometries, materials, node attributes
    const auto model = [&](const std::string &name, const char *type, std::array<double, 3> position = {},
                           std::array<double, 3> rotation = {}, std::vector<Node> extra = {}) {
        const int64_t id = next_id++;
        Node props{"Properties70",
                   {},
                   {P_vec("Lcl Translation", "Lcl Translation", position),
                    P_vec("Lcl Rotation", "Lcl Rotation", rotation), P_vec("Lcl Scaling", "Lcl Scaling", {1, 1, 1})}};
        for (Node &e : extra)
            props.kids.push_back(std::move(e));
        objects.kids.push_back({"Model",
                                {L(id), S(object_name(name, "Model")), S(type)},
                                {{"Version", {I(232)}, {}}, std::move(props), {"Culling", {S("CullingOff")}, {}}}});
        ++counts[0];
        return id;
    };
    std::map<std::string, int64_t> groups;
    const auto group = [&](const std::string &name) -> int64_t {
        if (name.empty())
            return 0;
        if (const auto it = groups.find(name); it != groups.end())
            return it->second;
        const int64_t id = model(name, "Null");
        connect(id, 0);
        return groups[name] = id;
    };

    for (const FbxMesh &m : meshes) {
        // Shared corners keep faces connected in DCC tools; normals are left to the importer.
        std::vector<double> positions;
        std::vector<int32_t> index;
        std::map<std::array<float, 3>, int32_t> seen;
        for (std::size_t i = 0; i + 2 < m.vertices.size(); i += 3) {
            const std::array<float, 3> p{m.vertices[i], m.vertices[i + 1], m.vertices[i + 2]};
            auto [it, fresh] = seen.try_emplace(p, int32_t(seen.size()));
            if (fresh)
                positions.insert(positions.end(), {p[0], p[1], p[2]});
            index.push_back((i / 3) % 3 == 2 ? ~it->second : it->second); // last corner of a polygon is negated
        }
        const int64_t parent = group(m.parent);
        const int64_t mid = model(m.name, "Mesh");
        connect(mid, parent);

        const int64_t gid = next_id++;
        Node geometry{"Geometry",
                      {L(gid), S(object_name(m.name, "Geometry")), S("Mesh")},
                      {{"Vertices", {array('d', positions)}, {}},
                       {"PolygonVertexIndex", {array('i', index)}, {}},
                       {"GeometryVersion", {I(124)}, {}},
                       {"LayerElementMaterial",
                        {I(0)},
                        {{"Version", {I(101)}, {}},
                         {"Name", {S("")}, {}},
                         {"MappingInformationType", {S("AllSame")}, {}},
                         {"ReferenceInformationType", {S("IndexToDirect")}, {}},
                         {"Materials", {array('i', std::vector<int32_t>{0})}, {}}}}}};
        Node layer{"Layer",
                   {I(0)},
                   {{"Version", {I(100)}, {}},
                    {"LayerElement", {}, {{"Type", {S("LayerElementMaterial")}, {}}, {"TypedIndex", {I(0)}, {}}}}}};
        if (!m.uvs.empty()) {
            std::vector<double> uv(m.uvs.begin(), m.uvs.end());
            geometry.kids.push_back({"LayerElementUV",
                                     {I(0)},
                                     {{"Version", {I(101)}, {}},
                                      {"Name", {S("UVMap")}, {}},
                                      {"MappingInformationType", {S("ByPolygonVertex")}, {}},
                                      {"ReferenceInformationType", {S("Direct")}, {}},
                                      {"UV", {array('d', uv)}, {}}}});
            layer.kids.push_back(
                {"LayerElement", {}, {{"Type", {S("LayerElementUV")}, {}}, {"TypedIndex", {I(0)}, {}}}});
        }
        geometry.kids.push_back(std::move(layer));
        objects.kids.push_back(std::move(geometry));
        connect(gid, mid);
        ++counts[1];

        const int64_t matid = next_id++;
        const std::array<double, 3> c{m.colour[0], m.colour[1], m.colour[2]};
        objects.kids.push_back(
            {"Material",
             {L(matid), S(object_name(m.name + (m.emissive ? "_screen" : "_material"), "Material")), S("")},
             {{"Version", {I(102)}, {}},
              {"ShadingModel", {S("phong")}, {}},
              {"MultiLayer", {I(0)}, {}},
              {"Properties70",
               {},
               {P_vec("DiffuseColor", "Color", c),
                P_vec("EmissiveColor", "Color", m.emissive ? c : std::array<double, 3>{}),
                P("EmissiveFactor", "Number", "", "A", {D(m.emissive ? 1 : 0)})}}}});
        connect(matid, mid);
        ++counts[2];
    }

    for (const FbxCamera &cam : cameras) {
        // FBX cameras look down local +X with +Y up. Rotation order ZYX (5): R = Ry(yaw)·Rz(pitch).
        std::array<double, 3> d{cam.target[0] - cam.position[0], cam.target[1] - cam.position[1],
                                cam.target[2] - cam.position[2]};
        const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (double &x : d)
            x = len > 0 ? x / len : 0;
        if (len == 0)
            d = {0, 0, -1};
        const double deg = 180 / std::numbers::pi;
        const double yaw = std::atan2(-d[2], d[0]) * deg, pitch = std::asin(std::clamp(d[1], -1.0, 1.0)) * deg;
        const double film_h = 0.797; // inches, a 16:9 back of a 36 mm sensor
        const double focal = film_h * 25.4 / 2 / std::tan(cam.fov / 2 / deg);
        const int64_t mid =
            model(cam.name, "Camera", cam.position, {0, yaw, pitch},
                  {P("RotationOrder", "enum", "", "", {I(5)}), P("RotationActive", "bool", "", "", {I(1)})});
        connect(mid, group("Cameras"));
        const int64_t aid = next_id++;
        objects.kids.push_back(
            {"NodeAttribute",
             {L(aid), S(object_name(cam.name, "NodeAttribute")), S("Camera")},
             {{"Properties70",
               {},
               {P("ApertureMode", "enum", "", "", {I(2)}), // vertical field of view
                P("FieldOfView", "FieldOfView", "", "A", {D(cam.fov)}), P("FilmWidth", "Number", "", "", {D(1.417)}),
                P("FilmHeight", "Number", "", "", {D(film_h)}), P("FocalLength", "Number", "", "A", {D(focal)})}},
              {"TypeFlags", {S("Camera")}, {}},
              {"GeometryVersion", {I(124)}, {}}}});
        connect(aid, mid);
        ++counts[3];
    }

    const auto type = [](const char *name, int count) {
        return Node{"ObjectType", {S(name)}, {{"Count", {I(count)}, {}}}};
    };
    const Node top[] = {{"FBXHeaderExtension",
                         {},
                         {{"FBXHeaderVersion", {I(1003)}, {}},
                          {"FBXVersion", {I(7400)}, {}},
                          {"Creator", {S("Scene Visualiser")}, {}}}},
                        {"Creator", {S("Scene Visualiser")}, {}},
                        {"GlobalSettings",
                         {},
                         {{"Version", {I(1000)}, {}},
                          {"Properties70",
                           {},
                           {P_int("UpAxis", 1), P_int("UpAxisSign", 1), P_int("FrontAxis", 2),
                            P_int("FrontAxisSign", 1), P_int("CoordAxis", 0), P_int("CoordAxisSign", 1),
                            P_int("OriginalUpAxis", 1), P_int("OriginalUpAxisSign", 1),
                            P_double("UnitScaleFactor", 100), P_double("OriginalUnitScaleFactor", 100)}}}},
                        {"Definitions",
                         {},
                         {{"Version", {I(100)}, {}},
                          {"Count", {I(counts[0] + counts[1] + counts[2] + counts[3])}, {}},
                          type("Model", counts[0]),
                          type("Geometry", counts[1]),
                          type("Material", counts[2]),
                          type("NodeAttribute", counts[3])}},
                        objects,
                        connections};

    std::string out("Kaydara FBX Binary  \0\x1a\0", 23);
    put(out, uint32_t(7400));
    for (const Node &n : top)
        write(out, n);
    out.append(13, '\0');
    // Footer as the FBX SDK writes it: id, padding to 16, version, zeros, magic.
    out.append("\xfa\xbc\xab\x09\xd0\xc8\xd4\x66\xb1\x76\xfb\x83\x1c\xf7\x26\x7e", 16);
    out.append(4, '\0');
    out.append((16 - out.size() % 16) % 16, '\0');
    put(out, uint32_t(7400));
    out.append(120, '\0');
    out.append("\xf8\x5a\x8c\x6a\xde\xf5\xd9\x7e\xec\xe9\x0c\xe3\x75\x8f\x29\x0b", 16);
    return {out.begin(), out.end()};
}

} // namespace sv
