#include "render/scene_builder.h"

#include <QColor>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace sv::render {

namespace {

constexpr float deg = std::numbers::pi_v<float> / 180;
const QVector3D accent(0x4c / 255.f, 0x8d / 255.f, 0xff / 255.f); // Theme accent: selection
const QVector4D truss_colour(0xa8 / 255.f, 0xad / 255.f, 0xb8 / 255.f, 1);
const QVector4D deck_colour(0x23 / 255.f, 0x26 / 255.f, 0x2e / 255.f, 1);
const QVector4D wall_back(0x15 / 255.f, 0x17 / 255.f, 0x1c / 255.f, 1);
const QVector4D piece_colour(0x6b / 255.f, 0x70 / 255.f, 0x80 / 255.f, 1);

QVector3D vec3(const json &j, QVector3D fallback = {}) {
    if (!j.is_array() || j.size() != 3)
        return fallback;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

QVector4D tinted(QVector4D c, bool selected) {
    return selected ? QVector4D(c.toVector3D() * 0.4f + accent * 0.6f, c.w()) : c;
}

QMatrix4x4 local_transform(const json &o) {
    QMatrix4x4 m;
    m.translate(vec3(o.value("position", json())));
    const QVector3D r = vec3(o.value("rotation", json()));
    m.rotate(r.y(), 0, 1, 0);
    m.rotate(r.x(), 1, 0, 0);
    m.rotate(r.z(), 0, 0, 1);
    m.scale(vec3(o.value("scale", json()), {1, 1, 1}));
    return m;
}

QMatrix4x4 box(const QMatrix4x4 &parent, QVector3D centre, QVector3D size) {
    QMatrix4x4 m = parent;
    m.translate(centre);
    m.scale(size);
    return m;
}

struct Builder {
    const json &project;
    const std::map<std::string, FixtureState> &fixtures;
    const std::set<std::string> &selected;
    double time;
    const MeshLibrary &meshes;
    RenderScene scene;
    std::map<std::string, const json *> by_uuid;

    QMatrix4x4 world(const json &o, int depth = 0) {
        QMatrix4x4 m = local_transform(o);
        const json &parent = o.value("parent", json());
        if (parent.is_string() && depth < 16)
            if (auto it = by_uuid.find(parent.get<std::string>()); it != by_uuid.end())
                return world(*it->second, depth + 1) * m;
        return m;
    }

    void add_beam(QVector3D origin, QVector3D dir, float beam_angle, QVector3D colour) {
        if (colour.length() < 0.004f)
            return;
        const float tan_half = std::tan(std::clamp(beam_angle, 0.5f, 120.0f) * deg / 2);
        float length = 25;
        if (dir.y() < -1e-3f && origin.y() > 0) {
            length = std::min(origin.y() / -dir.y(), 40.0f);
            const QVector3D hit = origin + dir * length;
            scene.spots.push_back({{hit.x(), hit.z()}, length * tan_half, colour});
        }
        scene.beams.push_back({origin, dir, length, tan_half, colour});
    }

    void fixture(const json &o, const std::string &uuid, bool sel) {
        const QMatrix4x4 m = world(o);
        const QColor housing(QString::fromStdString(o.value("housingColor", std::string("#202020"))));
        const QVector4D body_colour =
            tinted({float(housing.redF()), float(housing.greenF()), float(housing.blueF()), 1}, sel);
        auto it = fixtures.find(uuid);
        const FixtureState *st = it == fixtures.end() ? nullptr : &it->second;
        if (!st || !st->type) {
            scene.boxes.push_back({box(m, {0, -0.2f, 0}, {0.3f, 0.4f, 0.3f}), tinted({0.35f, 0.35f, 0.38f, 1}, sel)});
            scene.pick.push_back({box(m, {0, -0.2f, 0}, {0.3f, 0.4f, 0.3f}), uuid});
            return;
        }
        const auto &type = *st->type;
        const auto &a = st->attrs;
        const float L = float(type.body[0]), H = float(type.body[1]), W = float(type.body[2]);
        bool moving = false;
        for (const auto &ch : st->mode->channels)
            moving |= ch.attribute() == "Pan" || ch.attribute() == "Tilt";
        float strobe = 1;
        if (a.strobe_hz > 0)
            strobe = std::fmod(time * a.strobe_hz, 1.0) < 0.3 ? 1.0f : 0.0f;
        const float intensity = float(a.dimmer) * (a.shutter_open ? strobe : 0.0f);
        const QVector3D down(0, -1, 0);

        if (moving) {
            scene.boxes.push_back({box(m, {0, -0.1f * H, 0}, {L, 0.2f * H, W}), body_colour});
            QMatrix4x4 yoke = m;
            yoke.rotate(float(a.pan), 0, 1, 0);
            for (float side : {-1.f, 1.f})
                scene.boxes.push_back(
                    {box(yoke, {side * 0.45f * L, -0.45f * H, 0}, {0.08f * L, 0.5f * H, 0.3f * W}), body_colour});
            QMatrix4x4 head = yoke;
            head.translate(0, -0.6f * H, 0);
            head.rotate(-float(a.tilt), 1, 0, 0); // positive tilt swings the beam toward +Z (POC rotDir)
            scene.boxes.push_back({box(head, {0, -0.05f * H, 0}, {0.75f * L, 0.5f * H, 0.6f * W}), body_colour});
            const QVector3D origin = head.map(QVector3D(0, -0.3f * H, 0));
            const QVector3D dir = (head.mapVector(down)).normalized();
            add_beam(origin, dir, float(a.beam_angle), QVector3D(a.colour[0], a.colour[1], a.colour[2]) * intensity);
        } else {
            scene.boxes.push_back({box(m, {0, -0.5f * H, 0}, {L, H, W}), body_colour});
            const QVector3D dir = m.mapVector(down).normalized();
            if (a.cells.empty()) {
                add_beam(m.map(QVector3D(0, -H, 0)), dir, float(a.beam_angle),
                         QVector3D(a.colour[0], a.colour[1], a.colour[2]) * intensity);
            } else {
                const float n = float(a.cells.size());
                for (std::size_t i = 0; i < a.cells.size(); ++i) {
                    const auto &c = a.cells[i];
                    add_beam(m.map(QVector3D(-L / 2 + (float(i) + 0.5f) * L / n, -H, 0)), dir, float(a.beam_angle),
                             QVector3D(c[0], c[1], c[2]) * intensity);
                }
            }
        }
        scene.pick.push_back({box(m, {0, -0.5f * H, 0}, {L, H, std::max(W, L)}), uuid});
    }

    void truss(const json &o, const std::string &uuid, bool sel) {
        const QMatrix4x4 m = world(o);
        const float len = o.value("length", 3.0f), half = 0.12f, chord = 0.05f;
        const QVector4D c = tinted(truss_colour, sel);
        for (float y : {-half, half})
            for (float z : {-half, half})
                scene.boxes.push_back({box(m, {0, y, z}, {len, chord, chord}), c});
        for (float x : {-len / 2, len / 2})
            scene.boxes.push_back({box(m, {x, 0, 0}, {chord, 2 * half + chord, 2 * half + chord}), c});
        // shortcut: lacing is drawn as rungs every 0.5 m, not true diagonals; fine at previs distances.
        for (float x = -len / 2 + 0.5f; x < len / 2 - 0.01f; x += 0.5f)
            for (float y : {-half, half})
                scene.boxes.push_back({box(m, {x, y, 0}, {0.02f, 0.02f, 2 * half}), c});
        scene.pick.push_back({box(m, {}, {len, 2 * half + chord, 2 * half + chord}), uuid});
    }

    void wall(const json &o, const std::string &uuid, bool sel) {
        const QMatrix4x4 m = world(o);
        const float w = o.value("width", 4.0f), h = o.value("height", 2.5f);
        const auto derived = wall_derived(o);
        const json &comp = project.value("composition", json::object());
        const float cw = comp.value("width", 1920.0f), ch = comp.value("height", 1080.0f);
        const json &s = o.value("slice", json::object());
        QMatrix4x4 quad = m;
        quad.translate(0, h / 2, 0);
        quad.scale(w, h, 1);
        scene.walls.push_back(
            {quad,
             {s.value("x", 0.0f) / cw, s.value("y", 0.0f) / ch, s.value("w", cw) / cw, s.value("h", ch) / ch},
             {float(derived.pixels_x), float(derived.pixels_y), float(derived.modules_x), float(derived.modules_y)},
             std::clamp(o.value("nits", 1500.0f) / 1500.0f, 0.1f, 4.0f),
             sel});
        scene.boxes.push_back({box(m, {0, h / 2, -0.05f}, {w, h, 0.09f}), tinted(wall_back, sel)});
        scene.pick.push_back({box(m, {0, h / 2, -0.04f}, {w, h, 0.1f}), uuid});
    }

    void set_piece(const json &o, const std::string &uuid, bool sel) {
        std::string key = o.value("geometry", std::string());
        if (const std::string group = o.value("group", std::string()); !group.empty())
            key += "#" + group;
        const QMatrix4x4 m = world(o);
        const auto it = meshes.find(key);
        if (it == meshes.end()) { // geometry missing from the package: a marker box so it can be selected and fixed
            scene.boxes.push_back({box(m, {0, 0.5f, 0}, {1, 1, 1}), tinted({0.5f, 0.2f, 0.2f, 1}, sel)});
            scene.pick.push_back({box(m, {0, 0.5f, 0}, {1, 1, 1}), uuid});
            return;
        }
        const Mesh &mesh = *it->second;
        scene.meshes.push_back({it->second, m, tinted(piece_colour, sel)});
        scene.pick.push_back({box(m, (mesh.lo + mesh.hi) / 2, mesh.hi - mesh.lo), uuid});
    }

    void run() {
        const json &venue = project.value("venue", json::object());
        scene.venue_width = venue.value("width", 20.0f);
        scene.venue_depth = venue.value("depth", 15.0f);
        scene.haze = venue.value("haze", 0.35f);
        for (const json &d : venue.value("decks", json::array())) {
            const float h = d.value("height", 1.0f);
            const QVector3D p = vec3(d.value("position", json()));
            scene.boxes.push_back(
                {box({}, p + QVector3D(0, h / 2, 0), {d.value("width", 4.0f), h, d.value("depth", 2.0f)}),
                 deck_colour});
        }
        const json &objects = project.value("objects", json::array());
        for (const json &o : objects)
            if (o.contains("uuid") && o["uuid"].is_string())
                by_uuid[o["uuid"].get<std::string>()] = &o;
        for (std::size_t i = 0; i < objects.size(); ++i) {
            const json &o = objects[i];
            const std::size_t boxes = scene.boxes.size(), walls = scene.walls.size(), pieces = scene.meshes.size();
            const std::string uuid = o.value("uuid", std::string());
            const std::string type = o.value("type", std::string());
            const bool sel = selected.contains(uuid);
            if (type == "fixture")
                fixture(o, uuid, sel);
            else if (type == "truss")
                truss(o, uuid, sel);
            else if (type == "ledWall")
                wall(o, uuid, sel);
            else if (type == "setPiece")
                set_piece(o, uuid, sel);
            // Cameras are not drawn.
            for (auto k = boxes; k < scene.boxes.size(); ++k)
                scene.boxes[k].owner = int(i);
            for (auto k = walls; k < scene.walls.size(); ++k)
                scene.walls[k].owner = int(i);
            for (auto k = pieces; k < scene.meshes.size(); ++k)
                scene.meshes[k].owner = int(i);
        }
    }
};

bool hit_unit_box(const QMatrix4x4 &model, QVector3D origin, QVector3D dir, float &t_out) {
    bool invertible = false;
    const QMatrix4x4 inv = model.inverted(&invertible);
    if (!invertible)
        return false;
    const QVector3D o = inv.map(origin), d = inv.mapVector(dir);
    float t0 = -1e30f, t1 = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-9f) {
            if (std::abs(o[i]) > 0.5f)
                return false;
            continue;
        }
        float a = (-0.5f - o[i]) / d[i], b = (0.5f - o[i]) / d[i];
        if (a > b)
            std::swap(a, b);
        t0 = std::max(t0, a), t1 = std::min(t1, b);
    }
    if (t0 > t1 || t1 < 0)
        return false;
    t_out = t0 > 0 ? t0 : t1; // t is in local units but monotonic along the ray, so comparisons still order hits
    const QVector3D hit_world = model.map(o + d * t_out);
    t_out = (hit_world - origin).length();
    return true;
}

} // namespace

std::map<std::string, FixtureState> decode_fixtures(const json &project, const FixtureLibrary &library,
                                                    const FrameLookup &frame) {
    std::map<std::string, FixtureState> out;
    static const std::array<std::uint8_t, 512> blackout{};
    for (const json &o : project.value("objects", json::array())) {
        if (o.value("type", std::string()) != "fixture")
            continue;
        FixtureState st;
        auto it = library.find(o.value("gdtf", std::string()));
        if (it != library.end()) {
            st.type = &it->second;
            st.mode = it->second.mode(o.value("mode", std::string()));
            if (!st.mode)
                st.mode = &it->second.modes.front();
            std::span<const std::uint8_t> data = blackout;
            std::uint16_t address = 1;
            if (const json &p = o.value("patch", json()); p.is_object()) {
                if (auto f = frame(p.value("universe", std::uint16_t(1))); !f.empty())
                    data = f;
                address = p.value("address", std::uint16_t(1));
            }
            st.attrs = gdtf::decode(*st.type, *st.mode, data, address);
        }
        out[o.value("uuid", std::string())] = std::move(st);
    }
    return out;
}

std::shared_ptr<const Mesh> make_mesh(std::vector<float> vertices) {
    auto m = std::make_shared<Mesh>();
    m->lo = QVector3D(1e30f, 1e30f, 1e30f), m->hi = -m->lo;
    for (std::size_t i = 0; i + 5 < vertices.size(); i += 6) {
        const QVector3D p(vertices[i], vertices[i + 1], vertices[i + 2]);
        m->lo = QVector3D(std::min(m->lo.x(), p.x()), std::min(m->lo.y(), p.y()), std::min(m->lo.z(), p.z()));
        m->hi = QVector3D(std::max(m->hi.x(), p.x()), std::max(m->hi.y(), p.y()), std::max(m->hi.z(), p.z()));
    }
    if (vertices.empty())
        m->lo = m->hi = {};
    m->vertices = std::move(vertices);
    return m;
}

RenderScene build_scene(const json &project, const std::map<std::string, FixtureState> &fixtures,
                        const std::set<std::string> &selected, double time_s, const MeshLibrary &meshes) {
    Builder b{project, fixtures, selected, time_s, meshes, {}, {}};
    b.run();
    return std::move(b.scene);
}

std::vector<proto::GeneratorTarget> generator_targets(const json &project, const FixtureLibrary &library) {
    std::vector<proto::GeneratorTarget> out;
    for (const json &o : project.value("objects", json::array())) {
        const json &patch = o.value("patch", json());
        auto it = library.find(o.value("gdtf", std::string()));
        if (o.value("type", std::string()) != "fixture" || !patch.is_object() || it == library.end())
            continue;
        const gdtf::Mode *mode = it->second.mode(o.value("mode", std::string()));
        if (!mode)
            mode = &it->second.modes.front();
        proto::GeneratorTarget t{
            patch.value("universe", std::uint16_t(1)), patch.value("address", std::uint16_t(1)), {}, {}};
        // Cells in first-seen geometry order, as gdtf::decode reports them.
        std::vector<std::pair<std::string, std::map<std::string, std::uint16_t>>> by_geometry;
        for (const auto &ch : mode->channels) {
            if (ch.offsets.empty())
                continue;
            t.channels.try_emplace(ch.attribute(), ch.offsets.front());
            if (!ch.attribute().starts_with("ColorAdd_"))
                continue;
            auto g = std::ranges::find(by_geometry, ch.geometry, &decltype(by_geometry)::value_type::first);
            if (g == by_geometry.end())
                g = by_geometry.insert(g, {ch.geometry, {}});
            g->second[ch.attribute()] = ch.offsets.front();
        }
        if (by_geometry.size() > 1)
            for (auto &[geometry, c] : by_geometry)
                t.cells.push_back({c["ColorAdd_R"], c["ColorAdd_G"], c["ColorAdd_B"]});
        out.push_back(std::move(t));
    }
    return out;
}

std::string export_obj(const json &project, const FixtureLibrary &library, const MeshLibrary &meshes,
                       const std::set<std::string> &only) {
    const RenderScene scene = build_scene(
        project, decode_fixtures(project, library, [](std::uint16_t) { return std::span<const std::uint8_t>(); }), {},
        0, meshes);
    const json &objects = project.value("objects", json::array());
    // Per owner: triangles as world-space positions (normals are left to the importing tool).
    std::map<int, std::vector<QVector3D>> tris;
    static const auto cube = [] {
        std::vector<QVector3D> v;
        const QVector3D faces[6][3] = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
                                       {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
                                       {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}}, {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}}};
        for (const auto &[n, u, w] : faces)
            for (auto [a, b] : {std::pair{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}})
                v.push_back(0.5f * (n + float(a) * u + float(b) * w));
        return v;
    }();
    for (const BoxInstance &b : scene.boxes)
        for (const QVector3D &p : cube)
            tris[b.owner].push_back(b.model.map(p));
    for (const MeshInstance &m : scene.meshes)
        for (std::size_t i = 0; i + 5 < m.mesh->vertices.size(); i += 6)
            tris[m.owner].push_back(
                m.model.map(QVector3D(m.mesh->vertices[i], m.mesh->vertices[i + 1], m.mesh->vertices[i + 2])));
    std::map<int, const WallInstance *> walls;
    for (const WallInstance &w : scene.walls)
        walls[w.owner] = &w;

    std::string out = "# Scene Visualiser OBJ export: metres, Y-up, one group per scene object\n";
    std::size_t next = 1; // OBJ indices are 1-based and global
    const auto emit = [&](std::string name, const std::vector<QVector3D> &v, const char *material,
                          std::size_t corners = 3) {
        std::ranges::replace(name, ' ', '_'); // group names end at whitespace in most readers
        out += "g " + name + "\n";
        if (material)
            out += std::string("usemtl ") + material + "\n";
        char buf[96];
        for (const QVector3D &p : v) {
            std::snprintf(buf, sizeof buf, "v %.4f %.4f %.4f\n", p.x(), p.y(), p.z());
            out += buf;
        }
        for (std::size_t i = 0; i + corners <= v.size(); i += corners) {
            out += "f";
            for (std::size_t k = 0; k < corners; ++k)
                out += " " + std::to_string(next + i + k);
            out += "\n";
        }
        next += v.size();
    };
    const auto wanted = [&](int owner) {
        if (only.empty())
            return true;
        return owner >= 0 && only.contains(objects[std::size_t(owner)].value("uuid", std::string()));
    };
    if (wanted(-1) && tris.contains(-1))
        emit("Venue", tris[-1], nullptr);
    for (std::size_t i = 0; i < objects.size(); ++i) {
        if (!wanted(int(i)))
            continue;
        const std::string name = objects[i].value("name", std::string("Object ") + std::to_string(i + 1));
        if (const auto it = tris.find(int(i)); it != tris.end())
            emit(name, it->second, nullptr);
        if (const auto it = walls.find(int(i));
            it != walls.end()) { // the emissive face, named so renderers can texture it
            const QMatrix4x4 &m = it->second->model;
            emit(name + "_screen",
                 {m.map(QVector3D(-.5f, -.5f, 0)), m.map(QVector3D(.5f, -.5f, 0)), m.map(QVector3D(.5f, .5f, 0)),
                  m.map(QVector3D(-.5f, .5f, 0))},
                 "screen", 4);
        }
    }
    return out;
}

std::optional<std::string> pick(const RenderScene &scene, QVector3D origin, QVector3D direction) {
    std::optional<std::string> best;
    float best_t = 1e30f;
    for (const PickBox &p : scene.pick) {
        float t;
        if (hit_unit_box(p.model, origin, direction, t) && t < best_t)
            best_t = t, best = p.uuid;
    }
    return best;
}

} // namespace sv::render
