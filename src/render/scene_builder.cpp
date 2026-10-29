#include "render/scene_builder.h"

#include "core/fbx.h"
#include "render/camera.h"

#include <QColor>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
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

// A strings wall's `facing`: down, up, front (the audience, +Z), back, left (−X) or right (+X); anything else is
// all round (zero).
QVector3D string_facing(const std::string &f) {
    return f == "down"    ? QVector3D(0, -1, 0)
           : f == "up"    ? QVector3D(0, 1, 0)
           : f == "front" ? QVector3D(0, 0, 1)
           : f == "back"  ? QVector3D(0, 0, -1)
           : f == "left"  ? QVector3D(-1, 0, 0)
           : f == "right" ? QVector3D(1, 0, 0)
                          : QVector3D();
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

bool hit_unit_box(const QMatrix4x4 &model, QVector3D origin, QVector3D dir, float &t_out);

bool inside_unit_box(const QMatrix4x4 &model, QVector3D p) {
    bool invertible = false;
    const QVector3D l = model.inverted(&invertible).map(p);
    return invertible && std::abs(l.x()) <= 0.5f && std::abs(l.y()) <= 0.5f && std::abs(l.z()) <= 0.5f;
}

QMatrix4x4 box(const QMatrix4x4 &parent, QVector3D centre, QVector3D size) {
    QMatrix4x4 m = parent;
    m.translate(centre);
    m.scale(size);
    return m;
}

// Unit primitives for `shape` objects, centred in [-0.5, 0.5]^3, flat normals. Built once, so the GPU buffer is too.
void add_triangle(std::vector<float> &v, QVector3D a, QVector3D b, QVector3D c) {
    const QVector3D n = QVector3D::normal(a, b, c);
    for (const QVector3D &p : {a, b, c})
        v.insert(v.end(), {p.x(), p.y(), p.z(), n.x(), n.y(), n.z()});
}

std::shared_ptr<const Mesh> unit_cylinder() {
    static const auto mesh = [] {
        std::vector<float> v;
        constexpr int segments = 24;
        for (int i = 0; i < segments; ++i) {
            const float a0 = 2 * std::numbers::pi_v<float> * float(i) / segments,
                        a1 = 2 * std::numbers::pi_v<float> * float(i + 1) / segments;
            const QVector3D p0(0.5f * std::cos(a0), 0, -0.5f * std::sin(a0)),
                p1(0.5f * std::cos(a1), 0, -0.5f * std::sin(a1)), up(0, 0.5f, 0);
            add_triangle(v, p0 - up, p1 - up, p1 + up);
            add_triangle(v, p0 - up, p1 + up, p0 + up);
            add_triangle(v, up, p0 + up, p1 + up);
            add_triangle(v, -up, p1 - up, p0 - up);
        }
        return make_mesh(std::move(v));
    }();
    return mesh;
}

std::shared_ptr<const Mesh> unit_wedge() {
    static const auto mesh = [] {
        std::vector<float> v;
        const QVector3D a(-0.5f, -0.5f, -0.5f), b(0.5f, -0.5f, -0.5f), c(0.5f, -0.5f, 0.5f), d(-0.5f, -0.5f, 0.5f),
            e(-0.5f, 0.5f, -0.5f), f(0.5f, 0.5f, -0.5f);    // top edge along X at the back (-Z)
        add_triangle(v, a, c, b), add_triangle(v, a, d, c); // bottom
        add_triangle(v, a, b, f), add_triangle(v, a, f, e); // back
        add_triangle(v, d, e, f), add_triangle(v, d, f, c); // slope
        add_triangle(v, a, e, d), add_triangle(v, b, c, f); // sides
        return make_mesh(std::move(v));
    }();
    return mesh;
}

QMatrix4x4 world_of(const json &o, const std::map<std::string, const json *> &by_uuid, int depth = 0) {
    QMatrix4x4 m = local_transform(o);
    const json &parent = o.value("parent", json());
    if (parent.is_string() && depth < 16)
        if (auto it = by_uuid.find(parent.get<std::string>()); it != by_uuid.end())
            return world_of(*it->second, by_uuid, depth + 1) * m;
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

    QMatrix4x4 world(const json &o) { return world_of(o, by_uuid); }

    struct PendingBeam {
        QVector3D origin, dir;
        float tan_half;
        QVector3D colour;
        int owner;
    };
    std::vector<PendingBeam> pending;
    int current_owner = -1;
    float venue_height = 8;

    void add_beam(QVector3D origin, QVector3D dir, float beam_angle, QVector3D colour) {
        if (colour.length() < 0.004f)
            return;
        pending.push_back(
            {origin, dir, std::tan(std::clamp(beam_angle, 0.5f, 120.0f) * deg / 2), colour, current_owner});
    }

    // Beams end on the floor, at the venue height or on the first deck, shape, screen or set piece in their way.
    // shortcut: only a mostly downward beam leaves a pool of light; walls hit sideways get none.
    void resolve_beams() {
        std::vector<std::pair<const PickBox *, const char *>> solid;
        for (const PickBox &p : scene.pick)
            if (const auto it = by_uuid.find(p.uuid); it != by_uuid.end()) {
                const std::string t = it->second->value("type", std::string());
                for (const char *kind : {"deck", "shape", "ledWall", "setPiece"})
                    if (t == kind)
                        solid.push_back({&p, kind});
            }
        for (const PendingBeam &b : pending) {
            float length = 25;
            bool pool = false;
            const char *hit = "none";
            if (b.dir.y() < -1e-3f && b.origin.y() > 0) {
                length = b.origin.y() / -b.dir.y(), pool = true, hit = "floor";
                if (length > 40)
                    length = 40, hit = "none";
            } else if (b.dir.y() > 1e-3f && b.origin.y() < venue_height &&
                       (venue_height - b.origin.y()) / b.dir.y() < length)
                length = (venue_height - b.origin.y()) / b.dir.y(), hit = "ceiling";
            for (const auto &[p, kind] : solid) {
                float t;
                if (!inside_unit_box(p->model, b.origin) && hit_unit_box(p->model, b.origin, b.dir, t) && t < length)
                    length = t, pool = b.dir.y() < -0.5f, hit = kind;
            }
            if (pool) {
                const QVector3D hit = b.origin + b.dir * length;
                const float radius = std::max(length * b.tan_half, 0.05f);
                // The same light spread over a bigger pool is dimmer: full strength up to a 1.5 m radius.
                scene.spots.push_back({{hit.x(), hit.z()}, radius, b.colour * std::min(1.0f, 1.5f / radius)});
            }
            scene.beams.push_back({b.origin, b.dir, length, b.tan_half, b.colour, b.owner, hit});
        }
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
        // shortcut: brightness follows sqrt(flux) against a 10000 lm reference, clamped; use photometry if users ask.
        const float flux = std::clamp(float(std::sqrt(type.luminous_flux / 10000)), 0.25f, 1.0f);
        const float intensity = float(a.dimmer) * (a.shutter_open ? strobe : 0.0f) * flux;
        // shortcut: only the first beam's direction from the geometry chain, shared by every head; its offset and
        // the head pivot's height are ignored. Make it per head if a fixture mixes orientations.
        const QVector3D down(float(type.beam_direction[0]), float(type.beam_direction[1]),
                             float(type.beam_direction[2]));

        if (moving) {
            scene.boxes.push_back({box(m, {0, -0.1f * H, 0}, {L, 0.2f * H, W}), body_colour});
            // One yoke per head (TORNADO); a single-head fixture has no head list and one yoke at the body centre.
            const std::size_t n = std::max<std::size_t>(a.heads.size(), 1);
            const float hl = L / float(n);
            for (std::size_t h = 0; h < n; ++h) {
                const gdtf::HeadState *hs = a.heads.empty() ? nullptr : &a.heads[h];
                QMatrix4x4 yoke = m;
                if (hs) {
                    const auto &off = st->mode->heads[h].offset;
                    yoke.translate(float(off[0]), 0, float(off[2]));
                }
                yoke.rotate(float(hs ? hs->pan : a.pan), 0, 1, 0);
                for (float side : {-1.f, 1.f})
                    scene.boxes.push_back(
                        {box(yoke, {side * 0.45f * hl, -0.45f * H, 0}, {0.08f * hl, 0.5f * H, 0.3f * W}), body_colour});
                QMatrix4x4 head = yoke;
                head.translate(0, -0.6f * H, 0);
                // positive tilt swings the beam toward +Z (POC rotDir)
                head.rotate(-float(hs ? hs->tilt : a.tilt), 1, 0, 0);
                scene.boxes.push_back({box(head, {0, -0.05f * H, 0}, {0.75f * hl, 0.5f * H, 0.6f * W}), body_colour});
                const QVector3D origin = head.map(QVector3D(0, -0.3f * H, 0));
                const QVector3D dir = (head.mapVector(down)).normalized();
                const gdtf::Rgb &c = hs ? hs->colour : a.colour;
                add_beam(origin, dir, float(hs ? hs->beam_angle : a.beam_angle),
                         QVector3D(c[0], c[1], c[2]) * intensity);
            }
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
        const QVector4D rect{s.value("x", 0.0f) / cw, s.value("y", 0.0f) / ch, s.value("w", cw) / cw,
                             s.value("h", ch) / ch};
        const float nits = std::clamp(o.value("nits", 1500.0f) / 1500.0f, 0.1f, 4.0f);
        if (o.contains("strings") && o.at("strings").is_array() && !o.at("strings").empty()) {
            wall_strings(o.at("strings"), m, rect, derived.pixels_y, nits, uuid, sel,
                         string_facing(o.value("facing", std::string("all"))));
            return;
        }
        QMatrix4x4 quad = m;
        quad.translate(0, h / 2, 0);
        quad.scale(w, h, 1);
        scene.walls.push_back(
            {quad,
             rect,
             {float(derived.pixels_x), float(derived.pixels_y), float(derived.modules_x), float(derived.modules_y)},
             nits,
             sel,
             -1,
             s.value("angle", 0.0f),
             cw / ch});
        scene.boxes.push_back({box(m, {0, h / 2, -0.05f}, {w, h, 0.09f}), tinted(wall_back, sel)});
        scene.pick.push_back({box(m, {0, h / 2, -0.04f}, {w, h, 0.1f}), uuid});
    }

    // A wall built from LED strings (a mapped net): each string is a polyline in the wall's local space showing
    // one slice column, its pixels running from the first point (the slice's top) to the last.
    // `facing` is a world direction the LEDs face, turned about each segment; zero glows all round.
    // shortcut: the slice angle is ignored and OBJ/FBX export only the first segment, fine for upright columns.
    void wall_strings(const json &strings, const QMatrix4x4 &m, QVector4D rect, int rows, float nits,
                      const std::string &uuid, bool sel, QVector3D facing) {
        const float columns = float(strings.size()), thickness = 0.03f;
        for (std::size_t i = 0; i < strings.size(); ++i) {
            std::vector<QVector3D> p;
            for (const json &v : strings[i])
                p.push_back(m.map(vec3(v)));
            float total = 0;
            for (std::size_t k = 1; k < p.size(); ++k)
                total += (p[k] - p[k - 1]).length();
            if (total <= 0)
                continue;
            float run = 0;
            for (std::size_t k = 1; k < p.size(); ++k) {
                const QVector3D d = p[k - 1] - p[k]; // local +Y points back to the start, the slice's top
                const float len = d.length();
                if (len < 1e-4f)
                    continue;
                const QVector3D along = d / len;
                QVector3D across = QVector3D::crossProduct(along, {0, 1, 0});
                across = across.length() < 1e-3f ? QVector3D(1, 0, 0) : across.normalized();
                const QVector3D normal = QVector3D::crossProduct(across, along), mid = (p[k - 1] + p[k]) / 2;
                const auto frame = [&](QVector3D x, QVector3D z, float wide, float deep) {
                    x *= wide, z *= deep;
                    const QVector3D y = along * len;
                    return QMatrix4x4(x.x(), y.x(), z.x(), mid.x(), x.y(), y.y(), z.y(), mid.y(), x.z(), y.z(), z.z(),
                                      mid.z(), 0, 0, 0, 1);
                };
                // All round: two crossed quads, each also mirrored so its front faces both ways (a back face draws
                // dark). Facing one way: one quad turned toward `facing`, all round where the segment runs along it.
                std::vector<std::pair<QVector3D, QVector3D>> faces{
                    {across, normal}, {-across, -normal}, {normal, -across}, {-normal, across}};
                const QVector3D toward = facing - along * QVector3D::dotProduct(facing, along);
                if (toward.length() > 1e-3f)
                    faces = {{QVector3D::crossProduct(along, toward.normalized()), toward.normalized()}};
                for (const auto &[x, z] : faces)
                    scene.walls.push_back({frame(x, z, thickness, 1),
                                           {rect.x() + rect.z() * float(i) / columns, rect.y() + rect.w() * run / total,
                                            rect.z() / columns, rect.w() * len / total},
                                           {1, float(rows) * len / total, 1, 1},
                                           nits,
                                           sel});
                scene.pick.push_back({frame(across, normal, 0.1f, 0.1f), uuid});
                run += len;
            }
        }
    }

    // A bar along local X centred on the origin, one emissive cell per pixel; a slice-fed strip samples the
    // composition through the wall pass instead.
    void strip(const json &o, const std::string &uuid, bool sel) {
        const QMatrix4x4 m = world(o);
        const int px = std::max(1, o.value("pixels", 1)), rows = std::max(1, o.value("rows", 1));
        const float len = std::max(0.01f, o.value("length", 1.0f)), cell = len / float(px), depth = 0.03f;
        const float h = cell * float(rows);
        scene.boxes.push_back({box(m, {0, 0, -depth}, {len, h, depth}), tinted(wall_back, sel)});
        scene.pick.push_back({box(m, {0, 0, -depth / 2}, {len, std::max(h, 0.05f), 2 * depth}), uuid});
        if (o.value("pixelSource", std::string("dmx")) == "slice") {
            const json &comp = project.value("composition", json::object());
            const float cw = comp.value("width", 1920.0f), ch = comp.value("height", 1080.0f);
            const json &s = o.value("slice", json::object());
            QMatrix4x4 quad = m;
            quad.translate(0, 0, 0.001f);
            quad.scale(len, h, 1);
            scene.walls.push_back(
                {quad,
                 {s.value("x", 0.0f) / cw, s.value("y", 0.0f) / ch, s.value("w", cw) / cw, s.value("h", ch) / ch},
                 {float(px), float(rows), 1, 1},
                 1,
                 sel,
                 -1,
                 s.value("angle", 0.0f),
                 cw / ch});
            return;
        }
        const auto it = fixtures.find(uuid);
        for (int r = 0; r < rows; ++r)
            for (int i = 0; i < px; ++i) {
                const std::size_t k = std::size_t(r * px + i);
                QVector3D c;
                if (it != fixtures.end() && k < it->second.attrs.cells.size())
                    c = {it->second.attrs.cells[k][0], it->second.attrs.cells[k][1], it->second.attrs.cells[k][2]};
                // alpha 2: emissive, the mesh shader skips lighting
                scene.boxes.push_back(
                    {box(m, {-len / 2 + (float(i) + 0.5f) * cell, h / 2 - (float(r) + 0.5f) * cell, 0},
                         {cell * 0.8f, cell * 0.8f, depth}),
                     {c.x(), c.y(), c.z(), 2}});
            }
    }

    void deck(const json &o, const std::string &uuid, bool sel) {
        const float h = o.value("height", 1.0f);
        const QMatrix4x4 m = box(world(o), {0, h / 2, 0}, {o.value("width", 4.0f), h, o.value("depth", 2.0f)});
        scene.boxes.push_back({m, tinted(deck_colour, sel)});
        scene.pick.push_back({m, uuid});
    }

    void shape(const json &o, const std::string &uuid, bool sel) {
        const QVector3D size = vec3(o.value("size", json()), {1, 1, 1});
        const QMatrix4x4 m = box(world(o), {0, size.y() / 2, 0}, size);
        const QColor c(QString::fromStdString(o.value("color", std::string("#6b7080"))));
        const QVector4D colour = tinted({float(c.redF()), float(c.greenF()), float(c.blueF()), 1}, sel);
        const std::string kind = o.value("shape", std::string("box"));
        if (kind == "cylinder" || kind == "wedge")
            scene.meshes.push_back({kind == "cylinder" ? unit_cylinder() : unit_wedge(), m, colour});
        else
            scene.boxes.push_back({m, colour});
        scene.pick.push_back({m, uuid});
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
        scene.meshes.push_back({it->second, m, tinted(mesh.colour.w() > 0 ? mesh.colour : piece_colour, sel)});
        scene.pick.push_back({box(m, (mesh.lo + mesh.hi) / 2, mesh.hi - mesh.lo), uuid});
    }

    void run() {
        const json &venue = project.value("venue", json::object());
        scene.venue_width = venue.value("width", 20.0f);
        scene.venue_depth = venue.value("depth", 15.0f);
        scene.haze = venue.value("haze", 0.35f);
        // shortcut: haze follows the machines instantly; add a fill and decay time when shows need it
        for (const auto &[uuid, f] : fixtures)
            scene.haze = std::max(scene.haze, 0.8f * float(f.attrs.haze));
        venue_height = venue.value("height", 8.0f);
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
            current_owner = int(i);
            if (type == "fixture")
                fixture(o, uuid, sel);
            else if (type == "truss")
                truss(o, uuid, sel);
            else if (type == "ledWall")
                wall(o, uuid, sel);
            else if (type == "pixelStrip")
                strip(o, uuid, sel);
            else if (type == "setPiece")
                set_piece(o, uuid, sel);
            else if (type == "deck")
                deck(o, uuid, sel);
            else if (type == "shape")
                shape(o, uuid, sel);
            // Cameras are not drawn.
            for (auto k = boxes; k < scene.boxes.size(); ++k)
                scene.boxes[k].owner = int(i);
            for (auto k = walls; k < scene.walls.size(); ++k)
                scene.walls[k].owner = int(i);
            for (auto k = pieces; k < scene.meshes.size(); ++k)
                scene.meshes[k].owner = int(i);
        }
        resolve_beams();
        gizmo();
    }

    // FR-5 combined move/rotate/scale handles on the one selected object, in its parent's axes at its origin.
    // The viewport asks for them with a "gizmo:<all|move|rotate|scale>" entry in the selection; owner stays -1.
    // shortcut: fixed 1 m handles drawn with depth test; scale with camera distance and draw on top when rigs need it.
    void gizmo() {
        std::string mode;
        const json *one = nullptr;
        int count = 0;
        for (const std::string &s : selected)
            if (s.starts_with("gizmo:"))
                mode = s.substr(6);
            else if (const auto it = by_uuid.find(s); it != by_uuid.end())
                one = it->second, ++count;
        if (mode.empty() || count != 1)
            return;
        const QMatrix4x4 w = world(*one);
        QMatrix4x4 r = w * local_transform(*one).inverted(); // the parent's world transform
        r.setColumn(3, QVector4D(w.map(QVector3D()), 1));
        for (int c = 0; c < 3; ++c) // parent axes without its scale
            r.setColumn(c, QVector4D(r.column(c).toVector3D().normalized(), 0));
        // Lifted onto the object's top along the parent's up axis, so decks and shapes do not bury it.
        const QVector3D origin = r.column(3).toVector3D(), up = r.column(1).toVector3D();
        const std::string id = one->value("uuid", std::string());
        float lift = 0;
        for (const PickBox &p : scene.pick)
            if (p.uuid == id)
                for (int k = 0; k < 8; ++k)
                    lift = std::max(
                        lift, QVector3D::dotProduct(p.model.map(QVector3D(k & 1 ? 0.5f : -0.5f, k & 2 ? 0.5f : -0.5f,
                                                                          k & 4 ? 0.5f : -0.5f)) -
                                                        origin,
                                                    up));
        r.setColumn(3, QVector4D(origin + up * (lift + 0.05f), 1));
        scene.gizmo = r;
        const auto handle = [&](QVector3D centre, QVector3D size, QVector4D colour, const char *name) {
            scene.boxes.push_back({box(r, centre, size), colour});
            scene.pick.push_back({box(r, centre, size + QVector3D(0.12f, 0.12f, 0.12f)), name});
        };
        if (mode == "all" || mode == "move") {
            static const char *names[] = {"gizmo:tx", "gizmo:ty", "gizmo:tz"};
            for (int a = 0; a < 3; ++a) {
                QVector3D axis, bar(0.04f, 0.04f, 0.04f);
                axis[a] = 1;
                bar[a] = 1;
                QVector4D colour(0.25f, 0.25f, 0.25f, 1);
                colour[a] = 1;
                handle(axis * 0.55f, bar, colour, names[a]);
                handle(axis * 1.1f, {0.14f, 0.14f, 0.14f}, colour, names[a]);
            }
        }
        if (mode == "all" || mode == "rotate")
            for (const float sgn : {-1.f, 1.f}) {
                handle({0, 0, 0.8f * sgn}, {1.6f, 0.04f, 0.04f}, {1, 0.82f, 0.25f, 1}, "gizmo:ry");
                handle({0.8f * sgn, 0, 0}, {0.04f, 0.04f, 1.6f}, {1, 0.82f, 0.25f, 1}, "gizmo:ry");
            }
        if (mode == "all" || mode == "scale")
            handle({0.6f, 0.6f, 0.6f}, {0.16f, 0.16f, 0.16f}, {0.9f, 0.9f, 0.9f, 1}, "gizmo:s");
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

// Pixel colours of a DMX-driven strip, in Arena's colour order; W adds to all three.
std::vector<gdtf::Rgb> strip_cells(const json &o, const FrameLookup &frame) {
    const int n = std::max(0, o.value("pixels", 0)) * std::max(1, o.value("rows", 1));
    std::vector<gdtf::Rgb> cells(std::size_t(n), gdtf::Rgb{});
    if (!o.value("patch", json()).is_object() || o.value("pixelSource", std::string("dmx")) != "dmx")
        return cells;
    const std::string format = o.value("colorFormat", std::string("rgb"));
    std::map<int, std::span<const std::uint8_t>> frames;
    for (int i = 0; i < n; ++i) {
        const DmxAddress at = strip_pixel_address(o, i);
        auto it = frames.find(at.universe);
        if (it == frames.end())
            it = frames.emplace(at.universe, frame(std::uint16_t(at.universe))).first;
        for (std::size_t k = 0; k < format.size(); ++k) {
            const std::size_t slot = std::size_t(at.address - 1) + k;
            if (slot >= it->second.size())
                break;
            const float v = it->second[slot] / 255.0f;
            auto &c = cells[std::size_t(i)];
            switch (std::tolower(format[k])) {
            case 'r':
                c[0] += v;
                break;
            case 'g':
                c[1] += v;
                break;
            case 'b':
                c[2] += v;
                break;
            case 'w':
                c[0] += v, c[1] += v, c[2] += v;
                break;
            }
        }
    }
    return cells;
}

} // namespace

std::map<std::string, FixtureState> decode_fixtures(const json &project, const FixtureLibrary &library,
                                                    const FrameLookup &frame) {
    std::map<std::string, FixtureState> out;
    static const std::array<std::uint8_t, 512> blackout{};
    for (const json &o : project.value("objects", json::array())) {
        const std::string type = o.value("type", std::string());
        if (type == "pixelStrip") {
            out[o.value("uuid", std::string())].attrs.cells = strip_cells(o, frame);
            continue;
        }
        if (type != "fixture")
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

std::shared_ptr<const Mesh> make_mesh(std::vector<float> vertices, QVector4D colour) {
    auto m = std::make_shared<Mesh>();
    m->colour = colour;
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

QMatrix4x4 world_transform(const json &project, const json &object) {
    std::map<std::string, const json *> by_uuid;
    if (project.contains("objects")) // never value(): by_uuid points into the array
        for (const json &o : project.at("objects"))
            if (o.contains("uuid") && o["uuid"].is_string())
                by_uuid[o["uuid"].get<std::string>()] = &o;
    return world_of(object, by_uuid);
}

RenderScene build_scene(const json &project, const std::map<std::string, FixtureState> &fixtures,
                        const std::set<std::string> &selected, double time_s, const MeshLibrary &meshes) {
    Builder b{project, fixtures, selected, time_s, meshes, {}, {}};
    b.run();
    return std::move(b.scene);
}

namespace {
// The fixtures the generators drive: patched, with a known type.
bool generator_fixture(const json &o, const FixtureLibrary &library) {
    return o.value("type", std::string()) == "fixture" && o.value("patch", json()).is_object() &&
           library.contains(o.value("gdtf", std::string()));
}
} // namespace

std::vector<json> mapping_probes(const json &project, const FixtureLibrary &library) {
    struct Wall {
        QMatrix4x4 inverse, world;
        float width, height;
        json slice;
    };
    struct Segment { // one run of an LED string in world space: its slice column and its share of the string
        QVector3D a, b;
        double column, run0, run1; // 0..1 across and along the slice (the mapping wall_strings() draws with)
        json slice;
    };
    std::vector<Wall> walls;
    std::vector<Segment> segments;
    for (const json &o : project.value("objects", json::array())) {
        if (o.value("type", std::string()) != "ledWall")
            continue;
        const QMatrix4x4 m = world_transform(project, o);
        const json slice = o.value("slice", json::object());
        if (!(o.contains("strings") && o.at("strings").is_array() && !o.at("strings").empty())) {
            walls.push_back({m.inverted(), m, o.value("width", 4.0f), o.value("height", 2.5f), slice});
            continue;
        }
        const json &strings = o.at("strings");
        for (std::size_t i = 0; i < strings.size(); ++i) {
            std::vector<QVector3D> pts;
            for (const json &v : strings[i])
                pts.push_back(m.map(vec3(v)));
            float total = 0;
            for (std::size_t k = 1; k < pts.size(); ++k)
                total += (pts[k] - pts[k - 1]).length();
            if (total <= 0)
                continue;
            float run = 0;
            for (std::size_t k = 1; k < pts.size(); ++k) {
                const float len = (pts[k] - pts[k - 1]).length();
                segments.push_back({pts[k - 1], pts[k], (double(i) + 0.5) / double(strings.size()), run / total,
                                    (run + len) / total, slice});
                run += len;
            }
        }
    }
    const json comp = project.value("composition", json::object());
    const double cw = comp.value("width", 1920.0), ch = comp.value("height", 1080.0);
    const auto at = [&](const json &slice, double u, double v) { // slice origin top-left
        return json{{"x", slice.value("x", 0.0) + u * slice.value("w", cw)},
                    {"y", slice.value("y", 0.0) + v * slice.value("h", ch)},
                    {"w", 0},
                    {"h", 0}};
    };
    std::vector<json> out;
    for (const json &o : project.value("objects", json::array())) {
        if (!generator_fixture(o, library))
            continue;
        const QVector3D p = world_transform(project, o).map(QVector3D());
        json probe;
        float best = std::numeric_limits<float>::max();
        for (const Wall &w : walls) {
            const QVector3D l = w.inverse.map(p);
            // The wall quad is bottom-centre anchored: x in ±width/2, y in 0..height (see wall()).
            const QVector3D q(std::clamp(l.x(), -w.width / 2, w.width / 2), std::clamp(l.y(), 0.f, w.height), 0);
            const float d = (w.world.map(q) - p).lengthSquared();
            if (d >= best)
                continue;
            best = d;
            probe = at(w.slice, q.x() / w.width + 0.5, 1.0 - q.y() / w.height);
        }
        for (const Segment &sg : segments) {
            const QVector3D ab = sg.b - sg.a;
            const float l2 = ab.lengthSquared();
            const float s = l2 > 0 ? std::clamp(QVector3D::dotProduct(p - sg.a, ab) / l2, 0.f, 1.f) : 0.f;
            const float d = (sg.a + ab * s - p).lengthSquared();
            if (d >= best)
                continue;
            best = d;
            probe = at(sg.slice, sg.column, sg.run0 + (sg.run1 - sg.run0) * s);
        }
        out.push_back(std::move(probe));
    }
    return out;
}

std::vector<proto::GeneratorTarget> generator_targets(const json &project, const FixtureLibrary &library) {
    std::vector<proto::GeneratorTarget> out;
    for (const json &o : project.value("objects", json::array())) {
        if (!generator_fixture(o, library))
            continue;
        const json &patch = o.value("patch", json());
        auto it = library.find(o.value("gdtf", std::string()));
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

namespace {
// The scene at rest (no DMX) as world-space triangles per owner (index into objects), with the
// first instance colour, plus each wall's screen.
struct Gathered {
    std::map<int, std::vector<QVector3D>> tris;
    std::map<int, QVector4D> colour;
    std::map<int, const WallInstance *> walls;
    RenderScene scene;
};
void gather(Gathered &g, const json &project, const FixtureLibrary &library, const MeshLibrary &meshes) {
    g.scene = build_scene(
        project, decode_fixtures(project, library, [](std::uint16_t) { return std::span<const std::uint8_t>(); }), {},
        0, meshes);
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
    for (const BoxInstance &b : g.scene.boxes) {
        g.colour.try_emplace(b.owner, b.colour);
        for (const QVector3D &p : cube)
            g.tris[b.owner].push_back(b.model.map(p));
    }
    for (const MeshInstance &m : g.scene.meshes) {
        g.colour.try_emplace(m.owner, m.colour);
        for (std::size_t i = 0; i + 5 < m.mesh->vertices.size(); i += 6)
            g.tris[m.owner].push_back(
                m.model.map(QVector3D(m.mesh->vertices[i], m.mesh->vertices[i + 1], m.mesh->vertices[i + 2])));
    }
    for (const WallInstance &w : g.scene.walls)
        g.walls[w.owner] = &w;
}

bool wanted(const json &objects, const std::set<std::string> &only, int owner) {
    return only.empty() || (owner >= 0 && only.contains(objects[std::size_t(owner)].value("uuid", std::string())));
}
} // namespace

std::string export_obj(const json &project, const FixtureLibrary &library, const MeshLibrary &meshes,
                       const std::set<std::string> &only) {
    Gathered g;
    gather(g, project, library, meshes);
    const json &objects = project.value("objects", json::array());
    auto &tris = g.tris;
    auto &walls = g.walls;

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
    const auto wanted = [&](int owner) { return sv::render::wanted(objects, only, owner); };
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

std::vector<std::uint8_t> export_fbx(const json &project, const FixtureLibrary &library, const MeshLibrary &meshes,
                                     const std::set<std::string> &only) {
    Gathered g;
    gather(g, project, library, meshes);
    const json &objects = project.value("objects", json::array());
    static const std::map<std::string, std::string> folder{
        {"truss", "Trusses"},    {"ledWall", "Walls"},       {"pixelStrip", "Walls"},
        {"fixture", "Fixtures"}, {"setPiece", "Set pieces"}, {"shape", "Set pieces"}}; // decks fall to "Venue"
    std::vector<FbxMesh> out;
    std::vector<FbxCamera> cameras;
    const auto mesh = [&](std::string name, std::string parent, const std::vector<QVector3D> &v, QVector4D c) {
        FbxMesh m{std::move(name), std::move(parent), {}, {}, {c.x(), c.y(), c.z(), 1}, false};
        for (const QVector3D &p : v)
            m.vertices.insert(m.vertices.end(), {p.x(), p.y(), p.z()});
        out.push_back(std::move(m));
        return &out.back();
    };
    for (std::size_t i = 0; i < objects.size(); ++i) {
        if (!wanted(objects, only, int(i)))
            continue;
        const json &o = objects[i];
        const std::string name = o.value("name", std::string("Object ") + std::to_string(i + 1)),
                          type = o.value("type", std::string());
        const auto f = folder.find(type);
        const std::string parent = f == folder.end() ? "Venue" : f->second;
        if (const auto it = g.tris.find(int(i)); it != g.tris.end())
            mesh(name, parent, it->second, g.colour[int(i)]);
        if (const auto it = g.walls.find(int(i)); it != g.walls.end()) {
            // The emissive screen, UV-mapped to the wall's slice of the composition (FBX UV origin bottom-left).
            const WallInstance &w = *it->second;
            const QVector3D a = w.model.map(QVector3D(-.5f, -.5f, 0)), b = w.model.map(QVector3D(.5f, -.5f, 0)),
                            c = w.model.map(QVector3D(.5f, .5f, 0)), d = w.model.map(QVector3D(-.5f, .5f, 0));
            FbxMesh *s = mesh(name + "_screen", parent, {a, b, c, a, c, d}, {1, 1, 1, 1});
            s->emissive = true;
            const float u0 = w.slice.x(), u1 = w.slice.x() + w.slice.z(), v0 = 1 - w.slice.y() - w.slice.w(),
                        v1 = 1 - w.slice.y();
            s->uvs = {u0, v0, u1, v0, u1, v1, u0, v0, u1, v1, u0, v1};
        }
        if (type == "camera") {
            const auto vec = [&](const char *key, std::array<double, 3> fallback) {
                const json &v = o.value(key, json());
                return v.is_array() && v.size() == 3
                           ? std::array<double, 3>{v[0].get<double>(), v[1].get<double>(), v[2].get<double>()}
                           : fallback;
            };
            cameras.push_back({name, vec("position", {0, 1.7, 15}), vec("target", {0, 2, 0}), o.value("fov", 50.0)});
        }
    }
    if (only.empty()) { // viewport bookmarks become cameras too
        const json &layout = project.value("layout", json::object());
        int n = 0;
        if (layout.contains("cameraBookmarks") && layout.at("cameraBookmarks").is_array())
            for (const json &b : layout.at("cameraBookmarks")) {
                ++n;
                const json &t = b.value("target", json::array());
                if (!t.is_array() || t.size() != 3)
                    continue;
                OrbitCamera c;
                c.target = QVector3D(t[0].get<float>(), t[1].get<float>(), t[2].get<float>());
                c.yaw = b.value("yaw", 0.0f), c.pitch = b.value("pitch", 20.0f);
                c.distance = b.value("distance", 20.0f), c.fov = b.value("fov", 50.0f);
                const QVector3D e = c.eye();
                cameras.push_back({"Bookmark " + std::to_string(n),
                                   {e.x(), e.y(), e.z()},
                                   {c.target.x(), c.target.y(), c.target.z()},
                                   c.fov});
            }
    }
    return write_fbx(out, cameras);
}

std::optional<std::string> pick(const RenderScene &scene, QVector3D origin, QVector3D direction) {
    std::optional<std::string> best;
    float best_t = 1e30f;
    bool best_gizmo = false;
    for (const PickBox &p : scene.pick) {
        float t;
        const bool gizmo = p.uuid.starts_with("gizmo:"); // handles win over the body they sit in
        if (hit_unit_box(p.model, origin, direction, t) && (gizmo > best_gizmo || (gizmo == best_gizmo && t < best_t)))
            best_t = t, best = p.uuid, best_gizmo = gizmo;
    }
    return best;
}

} // namespace sv::render
