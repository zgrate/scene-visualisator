#include "render/drawing_painter.h"

#include <QDate>
#include <QFileInfo>
#include <QPainterPath>
#include <algorithm>
#include <climits>
#include <cmath>

namespace sv::render {

DrawingSheet sheet_from_json(const json &d) {
    DrawingSheet s;
    if (!d.is_object())
        return s;
    s.kind = d.value("kind", s.kind);
    s.paper = d.value("sheet", s.paper);
    s.scale = d.value("scale", 0);
    s.title = d.value("title", std::string());
    if (d.contains("hiddenLayers") && d["hiddenLayers"].is_array())
        for (const json &l : d["hiddenLayers"])
            if (l.is_string())
                s.hidden_layers.insert(l.get<std::string>());
    if (d.contains("dimensions") && d["dimensions"].is_array())
        s.dimensions = d["dimensions"];
    if (d.contains("notes") && d["notes"].is_array())
        s.notes = d["notes"];
    s.auto_dimensions = d.value("autoDimensions", true);
    s.labels = d.value("labels", true);
    s.legend = d.value("legend", true);
    s.schedule = d.value("schedule", false);
    return s;
}

QSizeF sheet_size_mm(const DrawingSheet &sheet) { return sheet.paper == "A4" ? QSizeF(297, 210) : QSizeF(420, 297); }

QPointF drawing_point(const std::string &kind, QVector3D p) {
    if (kind == "front")
        return {p.x(), -p.y()};
    if (kind == "side")
        return {-p.z(), -p.y()};
    return {p.x(), p.z()};
}

QVector3D drawing_delta(const std::string &kind, QPointF d) {
    if (kind == "front")
        return {float(d.x()), float(-d.y()), 0};
    if (kind == "side")
        return {0, float(-d.y()), float(-d.x())};
    return {float(d.x()), 0, float(d.y())};
}

namespace {

const QColor ink(0x1a, 0x1a, 0x1a), faint(0x8a, 0x8a, 0x8a), deck_fill(0xe9, 0xe9, 0xe6), piece_fill(0xf1, 0xf1, 0xee),
    screen(0x1f, 0x5f, 0xd6), dim_ink(0x40, 0x40, 0x40);

QPolygonF hull(std::vector<QPointF> pts) { // Andrew's monotone chain
    std::sort(pts.begin(), pts.end(),
              [](QPointF a, QPointF b) { return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y()); });
    const auto cross = [](QPointF o, QPointF a, QPointF b) {
        return (a.x() - o.x()) * (b.y() - o.y()) - (a.y() - o.y()) * (b.x() - o.x());
    };
    std::vector<QPointF> h(2 * pts.size());
    std::size_t k = 0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 1e-12)
            --k;
        h[k++] = pts[i];
    }
    for (std::size_t i = pts.size() - 1, t = k + 1; i-- > 0;) {
        while (k >= t && cross(h[k - 2], h[k - 1], pts[i]) <= 1e-12)
            --k;
        h[k++] = pts[i];
    }
    h.resize(k > 1 ? k - 1 : k);
    return QPolygonF(QList<QPointF>(h.begin(), h.end()));
}

// Text in page millimetres: the font is sized in tenths of a millimetre under a 0.1 scale, because pixel sizes are
// integers. Returns the text's bounding box in page millimetres; with `draw` false it only measures.
QRectF text(QPainter &p, QPointF at, const QString &s, double height_mm, int align, bool bold = false,
            bool draw = true) {
    p.save();
    p.scale(0.1, 0.1);
    QFont f(QStringLiteral("Sans Serif"));
    f.setPixelSize(std::max(1, int(std::lround(height_mm * 10))));
    f.setBold(bold);
    p.setFont(f);
    const int h = align & Qt::AlignLeft ? 0 : align & Qt::AlignRight ? 2 : 1;
    const int v = align & Qt::AlignTop ? 0 : align & Qt::AlignBottom ? 2 : 1;
    const QRectF r(at.x() * 10 - (h == 0   ? 0
                                  : h == 1 ? 2000
                                           : 4000),
                   at.y() * 10 - (v == 0   ? 0
                                  : v == 1 ? 2000
                                           : 4000),
                   4000, 4000);
    QRectF box = p.boundingRect(r, align, s);
    if (draw)
        p.drawText(r, align, s);
    p.restore();
    return QRectF(box.topLeft() / 10, box.size() / 10);
}

// Aligned dimension between page points a and b, its line offset by `offset` mm to the left of a->b.
// Returns the figure's box; with `draw` false it only measures.
QRectF dimension(QPainter &p, QPointF a, QPointF b, double offset, const QString &label, bool lettering,
                 bool draw = true) {
    const QPointF d = b - a;
    const double len = std::hypot(d.x(), d.y());
    if (len < 0.5)
        return {};
    const QPointF n(d.y() / len, -d.x() / len), u = d / len, o = n * offset;
    const QPointF a2 = a + o, b2 = b + o, gap = n * (offset > 0 ? 1.0 : -1.0);
    double angle = std::atan2(d.y(), d.x()) * 180 / M_PI;
    if (angle > 90.01 || angle < -89.99)
        angle += 180;
    QTransform t;
    t.translate(((a2 + b2) / 2).x(), ((a2 + b2) / 2).y());
    t.rotate(angle);
    if (!draw)
        return lettering ? t.mapRect(text(p, {0, -0.8}, label, 2.2, Qt::AlignHCenter | Qt::AlignBottom, false, false))
                         : QRectF();
    p.setPen(QPen(dim_ink, 0.18));
    p.drawLine(a + gap, a2 + gap * 1.5);
    p.drawLine(b + gap, b2 + gap * 1.5);
    p.drawLine(a2 - u * 1.5, b2 + u * 1.5);
    p.setPen(QPen(dim_ink, 0.35));
    const QPointF tick = (u + n) * 1.0;
    p.drawLine(a2 - tick, a2 + tick);
    p.drawLine(b2 - tick, b2 + tick);
    if (!lettering)
        return {};
    p.save();
    p.setTransform(t, true);
    const QRectF box = t.mapRect(text(p, {0, -0.8}, label, 2.2, Qt::AlignHCenter | Qt::AlignBottom));
    p.restore();
    return box;
}

// Arrow from `from` to `to`, head at `to`, in page millimetres.
void arrow(QPainter &p, QPointF from, QPointF to) {
    const QPointF d = to - from;
    const double len = std::hypot(d.x(), d.y());
    if (len < 0.5)
        return;
    p.drawLine(from, to);
    const QPointF u = d / len, n(-u.y(), u.x());
    p.setBrush(p.pen().color());
    p.drawPolygon(QPolygonF{to, to - u * 2 + n * 0.7, to - u * 2 - n * 0.7});
    p.setBrush(Qt::NoBrush);
}

QString metres(double m) { return QString::number(m, 'f', 2) + QStringLiteral(" m"); }

enum class Symbol { spot, wash, beam, blinder, pixel };
Symbol symbol_of(const gdtf::FixtureType *type) {
    if (!type)
        return Symbol::spot;
    std::string n = type->name + " " + type->short_name;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (auto [key, s] : {std::pair{"pixel", Symbol::pixel},
                          {"bar", Symbol::pixel},
                          {"blinder", Symbol::blinder},
                          {"wash", Symbol::wash},
                          {"beam", Symbol::beam}})
        if (n.find(key) != std::string::npos)
            return s;
    return Symbol::spot;
}

void fixture_symbol(QPainter &p, QPointF c, Symbol s, bool selected) {
    p.setPen(QPen(selected ? screen : ink, 0.3));
    p.setBrush(Qt::white);
    const double r = 2.2;
    switch (s) {
    case Symbol::wash:
        p.drawEllipse(c, r, r);
        p.setBrush(ink);
        p.drawEllipse(c, r * 0.45, r * 0.45);
        break;
    case Symbol::beam: {
        const QPolygonF tri{c + QPointF(0, -r), c + QPointF(r * 0.9, r * 0.7), c + QPointF(-r * 0.9, r * 0.7)};
        p.drawPolygon(tri);
        break;
    }
    case Symbol::blinder:
        p.drawRect(QRectF(c - QPointF(r * 1.3, r * 0.7), QSizeF(r * 2.6, r * 1.4)));
        p.drawEllipse(c - QPointF(r * 0.6, 0), r * 0.45, r * 0.45);
        p.drawEllipse(c + QPointF(r * 0.6, 0), r * 0.45, r * 0.45);
        break;
    case Symbol::pixel:
        p.drawRect(QRectF(c - QPointF(r * 1.8, r * 0.45), QSizeF(r * 3.6, r * 0.9)));
        for (int i = 1; i < 4; ++i)
            p.drawLine(c + QPointF(r * (-1.8 + 0.9 * i), -r * 0.45), c + QPointF(r * (-1.8 + 0.9 * i), r * 0.45));
        break;
    case Symbol::spot:
        p.drawEllipse(c, r, r);
        p.drawLine(c - QPointF(r, 0), c + QPointF(r, 0));
        p.drawLine(c - QPointF(0, r), c + QPointF(0, r));
        break;
    }
    p.setBrush(Qt::NoBrush);
}

struct Shape {
    QPolygonF poly; // drawing metres
    int owner;
    enum { deck, body, truss, piece, screen } role;
};

} // namespace

QTransform paint_drawing(QPainter &p, const json &project, const FixtureLibrary &library, const MeshLibrary &meshes,
                         const DrawingSheet &sheet, std::vector<DrawingHit> *hits) {
    const auto none = [](std::uint16_t) { return std::span<const std::uint8_t>(); };
    const auto fixtures = decode_fixtures(project, library, none);
    const RenderScene scene = build_scene(project, fixtures, {}, 0, meshes);
    const json &objects = project.contains("objects") ? project["objects"] : json::array();
    const auto object = [&](int owner) -> const json * {
        return owner >= 0 && std::size_t(owner) < objects.size() ? &objects[std::size_t(owner)] : nullptr;
    };
    const auto type_of = [&](int owner) {
        const json *o = object(owner);
        return o ? o->value("type", std::string()) : std::string("venue");
    };
    const auto hidden = [&](const json *o) {
        return o && o->contains("layer") && (*o)["layer"].is_string() &&
               sheet.hidden_layers.contains((*o)["layer"].get<std::string>());
    };
    const auto selected = [&](const json *o) {
        return o && !sheet.selected.empty() && sheet.selected.contains(o->value("uuid", std::string()));
    };
    const auto at = [&](QVector3D v) { return drawing_point(sheet.kind, v); };
    const auto project_all = [&](const QMatrix4x4 &m, QVector3D lo, QVector3D hi) {
        std::vector<QPointF> pts;
        for (int i = 0; i < 8; ++i)
            pts.push_back(
                at(m.map(QVector3D(i & 1 ? hi.x() : lo.x(), i & 2 ? hi.y() : lo.y(), i & 4 ? hi.z() : lo.z()))));
        return hull(std::move(pts));
    };

    // Shapes in drawing metres.
    std::vector<Shape> shapes;
    float deck_top = 0;
    for (const BoxInstance &b : scene.boxes) {
        const std::string type = type_of(b.owner);
        if (type == "fixture" || hidden(object(b.owner)))
            continue;
        if (type == "deck")
            deck_top = std::max(deck_top, b.model.map(QVector3D(0, 0.5f, 0)).y());
        shapes.push_back({project_all(b.model, QVector3D(-0.5f, -0.5f, -0.5f), QVector3D(0.5f, 0.5f, 0.5f)), b.owner,
                          type == "deck"                          ? Shape::deck
                          : type == "truss"                       ? Shape::truss
                          : type == "setPiece" || type == "shape" ? Shape::piece
                                                                  : Shape::body});
    }
    // shortcut: set pieces are drawn as their bounding box outline, project mesh silhouettes if users need them.
    for (const MeshInstance &m : scene.meshes)
        if (!hidden(object(m.owner)))
            shapes.push_back({project_all(m.model, m.mesh->lo, m.mesh->hi), m.owner, Shape::piece});
    for (const WallInstance &w : scene.walls)
        if (!hidden(object(w.owner)))
            shapes.push_back(
                {project_all(w.model, QVector3D(-0.5f, -0.5f, 0), QVector3D(0.5f, 0.5f, 0)), w.owner, Shape::screen});
    struct Placed {
        const json *o;
        QPointF c;
        QVector3D world;
        QRectF bounds; // drawing metres
    };
    std::vector<Placed> fixture_at, others_at;
    std::vector<DrawingHit> parts; // pick boxes of non-fixtures in drawing metres, hit one by one
    std::map<std::string, const json *> by_uuid;
    for (const json &o : objects)
        if (o.contains("uuid") && o["uuid"].is_string())
            by_uuid[o["uuid"].get<std::string>()] = &o;
    for (const PickBox &pb : scene.pick) {
        const auto it = by_uuid.find(pb.uuid);
        if (it == by_uuid.end() || hidden(it->second))
            continue;
        const QRectF r =
            project_all(pb.model, QVector3D(-0.5f, -0.5f, -0.5f), QVector3D(0.5f, 0.5f, 0.5f)).boundingRect();
        if (it->second->value("type", std::string()) == "fixture") {
            fixture_at.push_back({it->second, at(pb.model.map(QVector3D())), pb.model.map(QVector3D()), r});
            continue;
        }
        // One entry per object: a wall of LED strings has a pick box per segment.
        const auto same = std::find_if(others_at.begin(), others_at.end(), [&](const Placed &o) { return o.o == it->second; });
        if (same == others_at.end())
            others_at.push_back({it->second, at(pb.model.map(QVector3D())), pb.model.map(QVector3D()), r});
        else
            same->bounds |= r;
        parts.push_back({it->second->value("uuid", std::string()), r});
    }

    // Extent: everything drawn, plus the venue floor.
    QRectF stage;
    for (const Shape &s : shapes)
        stage |= s.poly.boundingRect();
    for (const Placed &f : fixture_at)
        stage |= QRectF(f.c - QPointF(0.2, 0.2), QSizeF(0.4, 0.4));
    const json &venue = project.contains("venue") ? project["venue"] : json::object();
    const double vw = venue.value("width", 20.0), vd = venue.value("depth", 15.0);
    QRectF floor = sheet.kind == "plan"    ? QRectF(-vw / 2, -vd / 2, vw, vd)
                   : sheet.kind == "front" ? QRectF(-vw / 2, 0, vw, 0)
                                           : QRectF(-vd / 2, 0, vd, 0);
    QRectF extent = stage.isNull() ? floor : stage | floor;
    if (extent.width() < 1 || extent.height() < 1)
        extent = extent.adjusted(-0.5, -0.5, 0.5, 0.5);

    // Page layout: 10 mm border, title block along the bottom right, 18 mm of room for dimensions around the view.
    const QSizeF page = sheet_size_mm(sheet);
    const QRectF frame(10, 10, page.width() - 20, page.height() - 20);
    const double block_h = 26;
    const double schedule_w = sheet.schedule ? 92 : 0;
    const QRectF area = frame.adjusted(22, 18, -18 - schedule_w, -block_h - 20);
    double mm_per_m = std::min(area.width() / extent.width(), area.height() / extent.height());
    int scale_n = sheet.scale;
    if (scale_n <= 0) { // the next standard scale that fits
        scale_n = 1000;
        for (int n : {10, 20, 25, 50, 75, 100, 125, 150, 200, 250, 500, 1000})
            if (1000.0 / n <= mm_per_m) {
                scale_n = n;
                break;
            }
    }
    mm_per_m = 1000.0 / scale_n;
    const QPointF centre = area.center() - extent.center() * mm_per_m;
    const QTransform to_page(mm_per_m, 0, 0, mm_per_m, centre.x(), centre.y());

    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(QRectF(QPointF(), page), Qt::white);
    p.setClipRect(frame);

    // Venue floor, then shapes back to front by role.
    p.setPen(QPen(faint, 0.25, Qt::DashLine));
    if (sheet.kind == "plan")
        p.drawRect(to_page.mapRect(floor));
    else
        p.drawLine(to_page.map(QPointF(floor.left(), 0)), to_page.map(QPointF(floor.right(), 0)));
    for (auto role : {Shape::deck, Shape::piece, Shape::body, Shape::truss, Shape::screen})
        for (const Shape &s : shapes) {
            if (s.role != role)
                continue;
            const QPolygonF poly = to_page.map(s.poly);
            switch (role) {
            case Shape::deck:
                p.setPen(QPen(ink, 0.35));
                p.setBrush(deck_fill);
                break;
            case Shape::piece:
                p.setPen(QPen(faint, 0.25));
                p.setBrush(piece_fill);
                break;
            case Shape::truss:
                p.setPen(QPen(ink, 0.2));
                p.setBrush(Qt::white);
                break;
            case Shape::screen:
                p.setPen(QPen(screen, 0.7));
                p.setBrush(QColor(screen.red(), screen.green(), screen.blue(), 40));
                break;
            default:
                p.setPen(QPen(ink, 0.25));
                p.setBrush(Qt::white);
            }
            if (selected(object(s.owner)))
                p.setPen(QPen(screen, 0.6));
            if (poly.boundingRect().width() < 0.3 || poly.boundingRect().height() < 0.3) { // edge-on: a line
                if (role == Shape::screen && !selected(object(s.owner))) // LED strings and strips: light, thin
                    p.setPen(QPen(QColor(screen.red(), screen.green(), screen.blue(), 110), 0.25));
                p.drawLine(poly.boundingRect().topLeft(), poly.boundingRect().bottomRight());
            } else
                p.drawPolygon(poly);
        }
    p.setBrush(Qt::NoBrush);

    // Fixture symbols, then labels: fixture IDs, patches, then truss and wall names. A label goes in the first of its
    // places that is free of symbols and earlier labels, or is left out, so dense rigs stay legible.
    std::vector<QRectF> taken;
    for (const Placed &f : fixture_at) {
        const auto it = library.find(f.o->value("gdtf", std::string()));
        const QPointF c = to_page.map(f.c);
        fixture_symbol(p, c, symbol_of(it == library.end() ? nullptr : &it->second), selected(f.o));
        taken.push_back(QRectF(c - QPointF(2, 2), QSizeF(4, 4)));
    }
    const auto label = [&](const QString &s, double height, bool bold,
                           std::initializer_list<std::pair<QPointF, int>> places) {
        for (const auto &[at, align] : places) {
            const QRectF box = text(p, at, s, height, align, bold, false);
            if (!frame.contains(box) ||
                std::any_of(taken.begin(), taken.end(), [&](const QRectF &t) { return t.intersects(box); }))
                continue;
            text(p, at, s, height, align, bold);
            taken.push_back(box);
            return;
        }
    };
    const int above = Qt::AlignHCenter | Qt::AlignBottom, below = Qt::AlignHCenter | Qt::AlignTop,
              right = Qt::AlignLeft | Qt::AlignVCenter, left = Qt::AlignRight | Qt::AlignVCenter;
    if (sheet.text && sheet.labels) {
        p.setPen(ink);
        for (const Placed &f : fixture_at) {
            const json &id = (*f.o)["fixtureId"];
            const QPointF c = to_page.map(f.c);
            if (id.is_number())
                label(QString::number(id.get<long long>()), 2.2, true,
                      {{c - QPointF(0, 2.4), above}, {c + QPointF(2.4, 0), right}, {c - QPointF(2.4, 0), left},
                       {c + QPointF(0, 2.4), below}});
        }
        for (const Placed &f : fixture_at) {
            const json &patch = f.o->contains("patch") ? (*f.o)["patch"] : json();
            const QPointF c = to_page.map(f.c);
            if (patch.is_object())
                label(QStringLiteral("%1.%2")
                          .arg(patch.value("universe", 0))
                          .arg(patch.value("address", 0), 3, 10, QChar('0')),
                      1.8, false, {{c + QPointF(0, 2.4), below}, {c + QPointF(2.4, 0), right}});
        }
        for (const Placed &o : others_at) {
            const std::string type = o.o->value("type", std::string());
            const QRectF r = to_page.mapRect(o.bounds);
            QString s = QString::fromStdString(o.o->value("name", std::string()));
            if (type == "truss")
                s += QStringLiteral(" · ") + metres(o.o->value("length", 0.0));
            else if (type == "ledWall") {
                const WallDerived d = wall_derived(*o.o);
                if (o.o->contains("strings") && (*o.o)["strings"].is_array() && !(*o.o)["strings"].empty())
                    s += QStringLiteral(" · %1 strings · %2 px").arg((*o.o)["strings"].size()).arg(d.pixels_y);
                else
                    s += QStringLiteral(" · %1 × %2 m · %3 × %4 px")
                         .arg(o.o->value("width", 0.0), 0, 'f', 2)
                         .arg(o.o->value("height", 0.0), 0, 'f', 2)
                         .arg(d.pixels_x)
                         .arg(d.pixels_y);
            } else
                continue;
            p.setPen(type == "ledWall" ? screen : ink);
            label(s, 2.2, false,
                  {{QPointF(r.center().x(), r.bottom() + 1.2), below},
                   {QPointF(r.center().x(), r.top() - 1.2), above},
                   {QPointF(r.right() + 1.2, r.center().y()), right},
                   {QPointF(r.left() - 1.2, r.center().y()), left}});
        }
    }

    // Automatic dimensions: overall stage, fixture spacing per parent row, truss trims in elevations.
    if (!stage.isNull() && sheet.auto_dimensions) {
        const QRectF s = to_page.mapRect(stage);
        dimension(p, s.bottomLeft(), s.bottomRight(), -8, metres(stage.width()), sheet.text);
        if (sheet.kind == "plan")
            dimension(p, s.bottomLeft(), s.topLeft(), -8, metres(stage.height()), sheet.text);
        else
            dimension(p, to_page.map(QPointF(stage.left(), 0)), to_page.map(QPointF(stage.left(), stage.top())), -8,
                      metres(-stage.top()), sheet.text);
    }
    // A row: same parent, same height and depth (10 cm), spread across the stage. Rows that project onto the same
    // positions (spots and washes on two trusses, seen from the front) get one chain.
    std::map<std::tuple<std::string, long, long>, std::vector<QPointF>> rows;
    if (sheet.kind != "side" && sheet.auto_dimensions)
        for (const Placed &f : fixture_at) {
            const json &parent = f.o->contains("parent") ? (*f.o)["parent"] : json();
            rows[{parent.is_string() ? parent.get<std::string>() : std::string(), std::lround(f.world.y() * 10),
                  std::lround(f.world.z() * 10)}]
                .push_back(f.c);
        }
    std::set<std::vector<long>> chained;
    for (auto &[key, pts] : rows) {
        std::sort(pts.begin(), pts.end(), [](QPointF a, QPointF b) { return a.x() < b.x(); });
        std::vector<long> signature;
        for (QPointF q : pts)
            signature.push_back(std::lround(q.x() * 100));
        if (pts.size() < 2 || !chained.insert(signature).second)
            continue;
        double top = pts.front().y();
        for (QPointF q : pts)
            top = std::min(top, q.y());
        for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
            const double gap = pts[i + 1].x() - pts[i].x();
            if (gap * mm_per_m < 8) // too short on paper for its figure
                continue;
            const QPointF a = to_page.map(QPointF(pts[i].x(), top)), b = to_page.map(QPointF(pts[i + 1].x(), top));
            const QRectF box = dimension(p, a, b, 6, metres(gap), sheet.text, false);
            if (std::any_of(taken.begin(), taken.end(), [&](const QRectF &t) { return t.intersects(box); }))
                continue;
            taken.push_back(dimension(p, a, b, 6, metres(gap), sheet.text));
        }
    }
    if (sheet.kind != "plan" && sheet.auto_dimensions)
        for (const Placed &o : others_at) {
            if (o.o->value("type", std::string()) != "truss")
                continue;
            const double trim = -o.bounds.bottom() - deck_top; // bottom chord above the deck top
            const QPointF a = to_page.map(QPointF(o.bounds.left(), -deck_top)),
                          b = to_page.map(QPointF(o.bounds.left(), o.bounds.bottom()));
            dimension(p, a, b, 4, QStringLiteral("trim ") + metres(trim), sheet.text);
        }
    for (const json &d : sheet.dimensions) {
        if (!d.is_object() || !d.contains("a") || !d.contains("b") || d["a"].size() != 2 || d["b"].size() != 2)
            continue;
        const QPointF a(d["a"][0].get<double>(), d["a"][1].get<double>()),
            b(d["b"][0].get<double>(), d["b"][1].get<double>());
        dimension(p, to_page.map(a), to_page.map(b), d.value("offset", 5.0),
                  metres(std::hypot(b.x() - a.x(), b.y() - a.y())), sheet.text);
    }

    // Notes: text at `at`, with a leader arrow to `to`.
    if (sheet.text)
        for (std::size_t i = 0; i < sheet.notes.size(); ++i) {
            const json &n = sheet.notes[i];
            if (!n.is_object() || !n.contains("at") || n["at"].size() != 2)
                continue;
            const QPointF a = to_page.map(QPointF(n["at"][0].get<double>(), n["at"][1].get<double>()));
            p.setPen(QPen(ink, 0.25));
            if (n.contains("to") && n["to"].size() == 2)
                arrow(p, a, to_page.map(QPointF(n["to"][0].get<double>(), n["to"][1].get<double>())));
            const QRectF box =
                text(p, a, QString::fromStdString(n.value("text", std::string())), 3, Qt::AlignLeft | Qt::AlignBottom);
            if (hits) // notes are picked by their text, at least 4 mm square so an empty one stays reachable
                hits->push_back({"note:" + std::to_string(i), box.united(QRectF(a - QPointF(0, 4), QSizeF(4, 4)))});
        }

    if (hits) {
        for (const Placed &f : fixture_at)
            hits->push_back(
                {f.o->value("uuid", std::string()), QRectF(to_page.map(f.c) - QPointF(2.2, 2.2), QSizeF(4.4, 4.4))});
        for (const DrawingHit &h : parts) // per box: the bounds of a wall of strings cover the stage between them
            hits->push_back({h.uuid, to_page.mapRect(h.page)});
    }

    // Border and title block.
    p.setClipping(false);
    p.setPen(QPen(ink, 0.5));
    p.drawRect(frame);
    const QRectF block(frame.right() - 150, frame.bottom() - block_h, 150, block_h);
    p.drawRect(block);
    p.setPen(QPen(ink, 0.25));
    p.drawLine(QPointF(block.left(), block.top() + 10), QPointF(block.right(), block.top() + 10));
    p.drawLine(QPointF(block.left() + 95, block.top() + 10), QPointF(block.left() + 95, block.bottom()));
    if (sheet.text) {
        static const std::map<std::string, QString> names{{"plan", QStringLiteral("Plan")},
                                                          {"front", QStringLiteral("Front elevation")},
                                                          {"side", QStringLiteral("Side elevation")}};
        const QString title = sheet.title.empty() ? names.count(sheet.kind) ? names.at(sheet.kind) : QString()
                                                  : QString::fromStdString(sheet.title);
        p.setPen(ink);
        text(p, block.topLeft() + QPointF(3, 5), QString::fromStdString(sheet.project_name), 4,
             Qt::AlignLeft | Qt::AlignVCenter, true);
        text(p, block.topLeft() + QPointF(3, 14), title, 3, Qt::AlignLeft | Qt::AlignVCenter, true);
        text(p, block.topLeft() + QPointF(3, 20),
             QStringLiteral("Scale 1:%1 · %2 · %3")
                 .arg(scale_n)
                 .arg(QString::fromStdString(sheet.paper), QDate::currentDate().toString(Qt::ISODate)),
             2.4, Qt::AlignLeft | Qt::AlignVCenter);
        QStringList layers;
        if (project.contains("layers") && project["layers"].is_array())
            for (const json &l : project["layers"])
                if (l.is_object() && !sheet.hidden_layers.contains(l.value("uuid", std::string())))
                    layers << QString::fromStdString(l.value("name", std::string()));
        text(p, block.topLeft() + QPointF(98, 12), QStringLiteral("LAYERS"), 2, Qt::AlignLeft | Qt::AlignTop, true);
        text(p, block.topLeft() + QPointF(98, 15.5),
             layers.isEmpty() ? QStringLiteral("All objects") : layers.join(u'\n'), 2, Qt::AlignLeft | Qt::AlignTop);
    }

    // Legend: one symbol per fixture type with its count, in columns left of the title block.
    if (sheet.text && sheet.legend && !fixture_at.empty()) {
        std::map<std::string, int> counts;
        for (const Placed &f : fixture_at)
            ++counts[f.o->value("gdtf", std::string())];
        const QRectF strip(frame.left(), block.top(), block.left() - frame.left(), block_h);
        const int per_column = 5, columns = std::max(1, int(strip.width() / 60));
        p.setPen(ink);
        text(p, strip.topLeft() + QPointF(3, 1), QStringLiteral("LEGEND"), 2, Qt::AlignLeft | Qt::AlignTop, true);
        int i = 0;
        for (const auto &[gdtf, n] : counts) {
            const QPointF at = strip.topLeft() + QPointF(3 + 60 * (i / per_column), 6.5 + 4.4 * (i % per_column));
            if (i == per_column * columns - 1 && counts.size() > std::size_t(i + 1)) {
                // shortcut: the strip holds 5 rows per 60 mm column, a separate legend sheet when rigs outgrow it
                text(p, at + QPointF(10, 0), QStringLiteral("+%1 more types").arg(counts.size() - i), 2.2,
                     Qt::AlignLeft | Qt::AlignVCenter);
                break;
            }
            const auto type = library.find(gdtf);
            const gdtf::FixtureType *t = type == library.end() ? nullptr : &type->second;
            fixture_symbol(p, at + QPointF(4.5, 0), symbol_of(t), false);
            p.setPen(ink);
            QString name = t ? QString::fromStdString(t->manufacturer + " " + t->name).trimmed()
                             : QFileInfo(QString::fromStdString(gdtf)).completeBaseName();
            if (name.size() > 34)
                name = name.left(33) + u'…';
            text(p, at + QPointF(10, 0), QStringLiteral("%1 × %2").arg(name).arg(n), 2.2,
                 Qt::AlignLeft | Qt::AlignVCenter);
            ++i;
        }
    }

    // Schedule: every fixture on this sheet, by fixture ID, in the column the drawing area left free.
    if (sheet.text && sheet.schedule) {
        const QRectF col(frame.right() - schedule_w, frame.top(), schedule_w, block.top() - frame.top());
        p.fillRect(col, Qt::white);
        p.setPen(QPen(ink, 0.25));
        p.drawLine(col.topLeft(), col.bottomLeft());
        p.setPen(ink);
        text(p, col.topLeft() + QPointF(3, 2), QStringLiteral("FIXTURE SCHEDULE"), 2, Qt::AlignLeft | Qt::AlignTop,
             true);
        std::vector<const json *> rows;
        for (const Placed &f : fixture_at)
            rows.push_back(f.o);
        std::stable_sort(rows.begin(), rows.end(), [](const json *a, const json *b) {
            const auto id = [](const json *o) {
                const json &v = o->contains("fixtureId") ? (*o)["fixtureId"] : json();
                return v.is_number() ? v.get<long long>() : LLONG_MAX;
            };
            return id(a) < id(b);
        });
        const double x[] = {3, 11, 39, 65, 79}; // ID, name, type, mode, patch
        const auto cell = [](std::string s, std::size_t n) {
            QString q = QString::fromStdString(s);
            return q.size() > qsizetype(n) ? q.left(qsizetype(n) - 1) + u'…' : q;
        };
        double y = col.top() + 7;
        for (const auto [c, h] : {std::pair{0, "ID"}, {1, "NAME"}, {2, "TYPE"}, {3, "MODE"}, {4, "PATCH"}})
            text(p, QPointF(col.left() + x[c], y), QString::fromLatin1(h), 1.6, Qt::AlignLeft | Qt::AlignVCenter, true);
        const std::size_t fit = std::size_t(std::max(0.0, (col.bottom() - y - 3) / 2.6));
        for (std::size_t r = 0; r < rows.size(); ++r) {
            y += 2.6;
            if (r + 1 == fit && rows.size() > fit) {
                // shortcut: one column on one sheet, a second schedule sheet when rigs outgrow it
                text(p, QPointF(col.left() + x[1], y), QStringLiteral("+%1 more").arg(rows.size() - r), 1.8,
                     Qt::AlignLeft | Qt::AlignVCenter);
                break;
            }
            const json &o = *rows[r];
            const auto type = library.find(o.value("gdtf", std::string()));
            const std::string tname = type == library.end() ? o.value("gdtf", std::string())
                                      : type->second.short_name.empty() ? type->second.name
                                                                        : type->second.short_name;
            const json &id = o.contains("fixtureId") ? o["fixtureId"] : json();
            const json &patch = o.contains("patch") ? o["patch"] : json();
            const QString cells[] = {
                id.is_number() ? QString::number(id.get<long long>()) : QString(),
                cell(o.value("name", std::string()), 16), cell(tname, 15), cell(o.value("mode", std::string()), 8),
                patch.is_object() ? QStringLiteral("%1.%2")
                                        .arg(patch.value("universe", 0))
                                        .arg(patch.value("address", 0), 3, 10, QChar('0'))
                                  : QString()};
            for (int c = 0; c < 5; ++c)
                text(p, QPointF(col.left() + x[c], y), cells[c], 1.8, Qt::AlignLeft | Qt::AlignVCenter);
        }
    }
    p.restore();
    return to_page;
}

} // namespace sv::render
