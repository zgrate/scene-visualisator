#include "viewport_item.h"

#include "render/scene_renderer.h"
#include "render/test_pattern.h"
#include "session.h"
#include "view_keys.h"

#include "render/transform_math.h"
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <numbers>
#include <rhi/qrhi.h>
#include <span>

namespace {

class ViewportRenderer : public QQuickRhiItemRenderer {
    void initialize(QRhiCommandBuffer *) override { scene.initialize(rhi(), renderTarget()); }

    void synchronize(QQuickRhiItem *item) override {
        view = static_cast<ViewportItem *>(item);
        Session *s = Session::instance();
        scene.set_camera(view->camera, double(view->clock.elapsed()) / 1000);
        if (const std::string g = view->gizmo_entry(); s->revision() != revision || g != gizmo) {
            gizmo = g;
            revision = s->revision();
            project = s->snapshot();
            library = s->library_snapshot();
            meshes = s->meshes();
            selection = s->selection();
            if (!gizmo.empty())
                selection.insert(gizmo);
            cpu_composition = s->composition();
            scene.set_composition(cpu_composition);
            composition_size = cpu_composition.size();
            base = {};
            media_serial = 0; // a live source's current frame goes back over the redrawn composition
            for (auto &[uuid, feed] : feeds)
                feed.serial = 0;
            buffers.clear();
            for (const QVariant &u : s->universes()) // every universe a fixture or strip reads
                buffers.try_emplace(std::uint16_t(u.toInt()), s->dmx().frames(std::uint16_t(u.toInt())));
        }
        if (auto sources = s->feed_sources();
            sources.size() != feeds.size() || !std::ranges::equal(sources, feeds, [](const auto &a, const auto &b) {
                return a.first == b.first && a.second == b.second.source;
            })) {
            feeds.clear();
            for (const auto &[uuid, source] : sources)
                feeds[uuid] = {source};
            base = {};
            media_serial = 0;
            scene.set_composition(cpu_composition);
        }
        if (auto sources = s->playback_feed_sources();
            sources.size() != recorded.size() ||
            !std::ranges::equal(sources, recorded, [](const auto &a, const auto &b) {
                return a.first == b.first && a.second == b.second.source;
            })) {
            recorded.clear();
            for (const auto &[uuid, source] : sources)
                recorded[uuid] = {source};
            base = {};
            media_serial = 0;
            scene.set_composition(cpu_composition);
        }
        if (auto opacity = s->playback_opacity(); opacity != recorded_opacity)
            recorded_opacity = std::move(opacity), opacity_changed = true;
        time = double(view->clock.elapsed()) / 1000;
    }

    void render(QRhiCommandBuffer *cb) override {
        bool fresh = false;
        if (QImage frame = Session::instance()->media().latest(media_serial); !frame.isNull()) {
            // Placed by source.fit unless it already matches the composition. shortcut: a CPU blit per frame;
            // scale in the wall shader when 4K sources make it show up in profiles (NFR-2).
            base = project && frame.size() != composition_size ? sv::render::make_composition(*project, frame)
                                                               : std::move(frame);
            fresh = true;
        }
        std::map<std::string, QImage> frames;
        for (auto &[uuid, feed] : feeds) {
            if (QImage frame = feed.source->latest(feed.serial); !frame.isNull())
                feed.frame = std::move(frame), fresh = true;
            if (!feed.frame.isNull())
                frames[uuid] = feed.frame;
        }
        std::map<std::string, QImage> recorded_frames;
        for (auto &[uuid, feed] : recorded) {
            if (QImage frame = feed.source->latest(feed.serial); !frame.isNull())
                feed.frame = std::move(frame), fresh = true;
            if (!feed.frame.isNull())
                recorded_frames[uuid] = feed.frame;
        }
        fresh = std::exchange(opacity_changed, false) || fresh;
        if (fresh && project)
            scene.set_composition(sv::render::compose_feeds(
                *project, sv::render::compose_feeds(*project, base.isNull() ? cpu_composition : base, frames),
                recorded_frames, recorded_opacity));
        const auto started = std::chrono::steady_clock::now();
        if (project) {
            for (auto &[u, b] : buffers)
                b->update();
            // shortcut: decodes and rebuilds every fixture each frame; cache per fixture when rigs reach 1000 (NFR-2).
            const auto fixtures = sv::render::decode_fixtures(*project, *library, [&](std::uint16_t u) {
                auto it = buffers.find(u);
                return it == buffers.end() ? std::span<const std::uint8_t>()
                                           : std::span<const std::uint8_t>(it->second->front().data);
            });
            view->animating = std::ranges::any_of(fixtures, [](const auto &f) {
                return f.second.attrs.strobe_hz > 0 && f.second.attrs.dimmer > 0;
            });
            auto built = sv::render::build_scene(*project, fixtures, selection, time, *meshes);
            std::set<int> lit;
            for (const auto &b : built.beams)
                lit.insert(b.owner);
            counts = {int(built.boxes.size()), int(built.meshes.size()), int(built.beams.size()),
                      int(built.walls.size()), int(lit.size())};
            scene.set_scene(std::move(built));
        }
        cpu_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        scene.render(cb, renderTarget());
        publish_stats();
    }

    // Debug mode's HUD and get_render_stats: averages over about half a second, published like the meter.
    void publish_stats() {
        ++frames;
        if (!stats_clock.isValid())
            stats_clock.start();
        if (stats_clock.elapsed() < 500)
            return;
        const auto info = rhi()->driverInfo();
        Session::instance()->publish_render_stats({{"fps", frames * 1000.0 / stats_clock.elapsed()},
                                                   {"cpuMs", cpu_ms / frames},
                                                   {"boxes", counts[0]},
                                                   {"meshes", counts[1]},
                                                   {"beams", counts[2]},
                                                   {"beamFixtures", counts[4]},
                                                   {"walls", counts[3]},
                                                   {"backend", QString::fromLatin1(rhi()->backendName())},
                                                   {"device", QString::fromUtf8(info.deviceName)},
                                                   {"revision", revision}});
        frames = 0;
        cpu_ms = 0;
        stats_clock.restart();
    }

    sv::render::SceneRenderer scene;
    ViewportItem *view = nullptr; // outlives its renderer
    quint64 revision = ~0ull;
    quint64 media_serial = 0;
    struct Feed {
        const sv::media::MediaSource *source = nullptr;
        quint64 serial = 0;
        QImage frame; // kept, so a quiet feed stays on its walls while another one updates
    };
    std::map<std::string, Feed> feeds;    // output uuid -> its live feed
    std::map<std::string, Feed> recorded; // output uuid -> the take's video, drawn over the live feed
    std::map<std::string, double> recorded_opacity;
    bool opacity_changed = false;
    QImage cpu_composition; // Session::composition() as of the last synchronize
    QImage base;            // the composition under the feeds: the main source's frame or the CPU one
    QSize composition_size;
    std::shared_ptr<const sv::json> project;
    std::shared_ptr<const sv::render::FixtureLibrary> library;
    std::shared_ptr<const sv::render::MeshLibrary> meshes;
    std::set<std::string> selection;
    std::string gizmo;
    std::map<std::uint16_t, sv::proto::TripleBuffer<sv::proto::UniverseFrame> *> buffers;
    double time = 0;
    QElapsedTimer stats_clock;
    int frames = 0;
    double cpu_ms = 0;
    std::array<int, 5> counts{}; // boxes, meshes, beams (one per pixel cell), walls, fixtures with a beam
};

} // namespace

ViewportItem::ViewportItem(QQuickItem *parent) : QQuickRhiItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton | Qt::RightButton);
    setAcceptHoverEvents(true); // modal G/R/S follow the cursor without a button held
    setSampleCount(4);
    clock.start();
    // Redraws when DMX, media, the document or a strobe changes; input handlers call update() themselves. Debug mode
    // always draws, so the HUD's frame rate means something.
    tick.setInterval(16);
    connect(&tick, &QTimer::timeout, this, [this] {
        if (fly_ != Fly::none)
            return step_fly();
        Session *s = Session::instance();
        std::vector<quint64> now{s->dmx().publish_serial(), s->revision(), s->media().serial()};
        for (const auto &[uuid, source] : s->feed_sources())
            now.push_back(source->serial());
        for (const auto &[uuid, source] : s->playback_feed_sources())
            now.push_back(source->serial());
        for (const auto &[uuid, opacity] : s->playback_opacity())
            now.push_back(quint64(opacity * 1e6));
        if (now != drawn_ || animating || s->debug_mode())
            drawn_ = std::move(now), update();
    });
    tick.start();
    // Hidden (covered by a screen or the 3D switch off): no ticks, so neither this item nor the window redraws.
    connect(this, &QQuickItem::visibleChanged, this, [this] { isVisible() ? tick.start() : tick.stop(); });
}

QQuickRhiItemRenderer *ViewportItem::createRenderer() { return new ViewportRenderer; }

std::string ViewportItem::gizmo_entry() const {
    return tool_ == "select" && !modal_ ? "gizmo:" + gizmo_mode_.toStdString() : std::string();
}

void ViewportItem::setGizmoMode(const QString &mode) {
    if (mode == gizmo_mode_ || !QStringList{"all", "move", "rotate", "scale"}.contains(mode))
        return;
    gizmo_mode_ = mode;
    Q_EMIT gizmoModeChanged();
    update();
}

void ViewportItem::setTool(const QString &tool) {
    if (tool == tool_)
        return;
    tool_ = tool;
    Q_EMIT toolChanged();
    update();
}

void ViewportItem::set_hint(const QString &hint) {
    if (hint != hint_)
        hint_ = hint, Q_EMIT transformHintChanged();
}

std::pair<QVector3D, QVector3D> ViewportItem::ray_at(QPointF p) const {
    return camera.ray(float(p.x() / width()), float(p.y() / height()), float(width() / height()));
}

std::optional<QVector3D> ViewportItem::on_plane(QPointF p, float y) const {
    const auto [origin, dir] = ray_at(p);
    return sv::render::plane_hit(origin, dir, {0, y, 0}, {0, 1, 0});
}

QPointF ViewportItem::to_screen(QVector3D world) const {
    const QVector3D ndc = (camera.projection(float(width() / height())) * camera.view()).map(world);
    return {(ndc.x() + 1) / 2 * width(), (1 - ndc.y()) / 2 * height()};
}

QVariantList ViewportItem::floorPoint(qreal x, qreal y) const {
    const auto at = on_plane({x, y}, 0);
    if (!at)
        return {};
    return {sv::render::snap(at->x(), 0.25), 0.0, sv::render::snap(at->z(), 0.25)};
}

namespace {
using sv::json;

// The scene as the viewport sees it, with the gizmo when asked; picking and framing use it.
sv::render::RenderScene scene_now(const std::string &gizmo) {
    Session *s = Session::instance();
    const auto blank = [](std::uint16_t) { return std::span<const std::uint8_t>(); };
    const auto fixtures = sv::render::decode_fixtures(*s->snapshot(), s->library(), blank);
    std::set<std::string> selection = s->selection();
    if (!gizmo.empty())
        selection.insert(gizmo);
    return sv::render::build_scene(*s->snapshot(), fixtures, selection, 0, *s->meshes());
}

const json *find_object(const json &project, const std::string &uuid) {
    for (const json &o : project.at("objects"))
        if (o.value("uuid", std::string()) == uuid)
            return &o;
    return nullptr;
}

QVector3D vec(const json &v, float fallback = 0) {
    return v.is_array() && v.size() == 3 ? QVector3D(v[0].get<float>(), v[1].get<float>(), v[2].get<float>())
                                         : QVector3D(fallback, fallback, fallback);
}
json arr(QVector3D v) {
    // 1e-4 m keeps float noise out of the file.
    const auto r = [](float x) { return std::round(double(x) * 1e4) / 1e4; };
    return {r(v.x()), r(v.y()), r(v.z())};
}
double wrap(double degrees) { return std::remainder(degrees, 360.0); }
} // namespace

bool ViewportItem::start_edit(const std::string &uuid, const std::string &handle,
                              const std::optional<QMatrix4x4> &frame) {
    Session *s = Session::instance();
    const json &doc = *s->snapshot();
    const json *o = find_object(doc, uuid);
    if (!o)
        return false;
    Edit e;
    e.uuid = uuid;
    e.handle = handle;
    e.kind = handle == "ry" ? "rotate" : handle == "s" ? "scale" : "move";
    e.start = {{"position", arr(vec(o->value("position", json())))},
               {"rotation", arr(vec(o->value("rotation", json())))},
               {"scale", arr(vec(o->value("scale", json()), 1))}};
    if (const json &p = o->value("parent", json()); p.is_string())
        if (const json *po = find_object(doc, p.get<std::string>()))
            e.parent = sv::render::world_transform(doc, *po);
    e.origin = sv::render::world_transform(doc, *o).map(QVector3D());
    if (frame)
        e.frame = *frame;
    else { // modal: parent axes at the object origin
        e.frame = e.parent;
        for (int c = 0; c < 3; ++c)
            e.frame.setColumn(c, QVector4D(e.frame.column(c).toVector3D().normalized(), 0));
        e.frame.setColumn(3, QVector4D(e.origin, 1));
    }
    edit_ = std::move(e);
    return true;
}

// A gizmo handle or body drag at cursor p.
void ViewportItem::drag_handle(QPointF p, bool free) {
    Edit &e = *edit_;
    const auto [ro, rd] = ray_at(p);
    const QVector3D fo = e.frame.column(3).toVector3D(), fy = e.frame.column(1).toVector3D();
    const QMatrix4x4 inv = e.parent.inverted();
    json fields;
    if (e.handle == "plane") {
        const auto hit = sv::render::plane_hit(ro, rd, e.origin, {0, 1, 0});
        if (!hit)
            return;
        QVector3D local = inv.map(e.origin + *hit - e.grab);
        if (!free)
            local = QVector3D(float(sv::render::snap(local.x(), 0.25)), local.y(),
                              float(sv::render::snap(local.z(), 0.25)));
        fields["position"] = arr(local);
        set_hint(QStringLiteral("Move %1, %2 m").arg(local.x(), 0, 'f', 2).arg(local.z(), 0, 'f', 2));
    } else if (e.handle == "tx" || e.handle == "ty" || e.handle == "tz") {
        const int a = e.handle[1] - 'x';
        const QVector3D axis = e.frame.column(a).toVector3D();
        const auto t = sv::render::axis_param(ro, rd, fo, axis);
        if (!t)
            return;
        double d = *t - e.t0;
        if (!free)
            d = sv::render::snap(d, 0.25);
        fields["position"] = arr(vec(e.start["position"]) + inv.mapVector(axis * float(d)));
        set_hint(QStringLiteral("Move %1: %2 m").arg(QChar('X' + a)).arg(d, 0, 'f', 2));
    } else {
        const auto hit = sv::render::plane_hit(ro, rd, fo, fy);
        if (!hit)
            return;
        const QVector3D v = *hit - fo;
        if (e.handle == "ry") {
            const double angle = std::atan2(-QVector3D::dotProduct(v, e.frame.column(2).toVector3D()),
                                            QVector3D::dotProduct(v, e.frame.column(0).toVector3D())) *
                                 180 / std::numbers::pi;
            double d = wrap(angle - e.t0);
            if (!free)
                d = sv::render::snap(d, 5);
            json r = e.start["rotation"];
            r[1] = wrap(r[1].get<double>() + d);
            fields["rotation"] = r;
            set_hint(QStringLiteral("Rotate Y: %1°").arg(d, 0, 'f', 1));
        } else {
            double ratio = v.length() / std::max(e.t0, 0.05);
            ratio = std::max(free ? ratio : sv::render::snap(ratio, 0.05), 0.01);
            fields["scale"] = arr(vec(e.start["scale"], 1) * float(ratio));
            set_hint(QStringLiteral("Scale: %1").arg(ratio, 0, 'f', 2));
        }
    }
    Session::instance()->preview_transform(e.uuid, fields);
}

// Blender's modal G/R/S at cursor p: the cursor sets the value unless a number was typed.
void ViewportItem::drag_modal(QPointF p, bool free) {
    Edit &e = *edit_;
    const std::optional<double> typed = sv::render::parse_typed(e.typed);
    const QPointF c = to_screen(e.origin);
    const QString axis_name = e.axis < 0 ? QString() : QString(QChar('X' + e.axis)) + " ";
    const QString suffix = e.typed.isEmpty() ? QString() : QStringLiteral("  [%1]").arg(e.typed);
    json fields;
    if (e.kind == "move") {
        const auto [ro, rd] = ray_at(p);
        const QVector3D forward = (camera.target - camera.eye()).normalized();
        const auto hit = sv::render::plane_hit(ro, rd, e.origin, forward);
        QVector3D d;
        if (typed) {
            d[std::max(e.axis, 0)] = float(*typed);
        } else if (hit) {
            d = *hit - e.grab;
            if (e.axis >= 0)
                d = QVector3D(e.axis == 0 ? d.x() : 0, e.axis == 1 ? d.y() : 0, e.axis == 2 ? d.z() : 0);
            if (!free)
                for (int i = 0; i < 3; ++i)
                    d[i] = float(sv::render::snap(d[i], 0.25));
        }
        fields["position"] = arr(vec(e.start["position"]) + e.parent.inverted().mapVector(d));
        set_hint(QStringLiteral("Move %1%2, %3, %4 m%5")
                     .arg(axis_name)
                     .arg(d.x(), 0, 'f', 2)
                     .arg(d.y(), 0, 'f', 2)
                     .arg(d.z(), 0, 'f', 2)
                     .arg(suffix));
    } else if (e.kind == "rotate") {
        const auto angle = [&](QPointF q) {
            return std::atan2(-(q.y() - c.y()), q.x() - c.x()) * 180 / std::numbers::pi;
        };
        double d = typed ? *typed : wrap(angle(p) - angle(e.mouse0));
        if (!typed && !free)
            d = sv::render::snap(d, 5);
        const int a = e.axis < 0 ? 1 : e.axis;
        json r = e.start["rotation"];
        r[a] = wrap(r[a].get<double>() + d);
        fields["rotation"] = r;
        set_hint(QStringLiteral("Rotate %1: %2°%3").arg(QChar('X' + a)).arg(d, 0, 'f', 1).arg(suffix));
    } else {
        const auto dist = [&](QPointF q) { return std::hypot(q.x() - c.x(), q.y() - c.y()); };
        double ratio = typed ? *typed : dist(p) / std::max(dist(e.mouse0), 1.0);
        if (!typed && !free)
            ratio = sv::render::snap(ratio, 0.05);
        ratio = std::max(ratio, 0.01);
        QVector3D sc = vec(e.start["scale"], 1);
        if (e.axis >= 0)
            sc[e.axis] *= float(ratio);
        else
            sc *= float(ratio);
        fields["scale"] = arr(sc);
        set_hint(QStringLiteral("Scale %1%2%3").arg(axis_name).arg(ratio, 0, 'f', 2).arg(suffix));
    }
    Session::instance()->preview_transform(e.uuid, fields);
}

void ViewportItem::finish(bool commit) {
    Session *s = Session::instance();
    if (edit_ && commit)
        s->commit_transform(edit_->kind == "move" ? "Move" : edit_->kind == "rotate" ? "Rotate" : "Scale");
    else
        s->cancel_transform();
    edit_.reset();
    modal_ = false;
    set_hint({});
    update();
}

bool ViewportItem::beginTransform(const QString &kind) {
    Session *s = Session::instance();
    const QString uuid = s->current().value("uuid").toString(); // shortcut: acts on the first selected object; move
                                                                // the whole selection when people ask for it
    if (uuid.isEmpty() || edit_ || !QStringList{"move", "rotate", "scale"}.contains(kind))
        return false;
    if (!start_edit(uuid.toStdString(), "modal", std::nullopt))
        return false;
    edit_->kind = kind.toStdString();
    edit_->mouse0 = last;
    const auto [ro, rd] = ray_at(last);
    edit_->grab = sv::render::plane_hit(ro, rd, edit_->origin, (camera.target - camera.eye()).normalized())
                      .value_or(edit_->origin);
    modal_ = true;
    forceActiveFocus();
    drag_modal(last, false);
    return true;
}

void ViewportItem::frameSelection(bool all) {
    const sv::render::RenderScene scene = scene_now({});
    const auto selection = all ? std::set<std::string>() : Session::instance()->selection();
    QVector3D lo(1e9f, 1e9f, 1e9f), hi = -lo;
    for (const auto &p : scene.pick)
        if (selection.empty() || selection.contains(p.uuid))
            for (int k = 0; k < 8; ++k) {
                const QVector3D c =
                    p.model.map(QVector3D(k & 1 ? 0.5f : -0.5f, k & 2 ? 0.5f : -0.5f, k & 4 ? 0.5f : -0.5f));
                for (int i = 0; i < 3; ++i)
                    lo[i] = std::min(lo[i], c[i]), hi[i] = std::max(hi[i], c[i]);
            }
    if (lo.x() > hi.x())
        return;
    camera.frame((lo + hi) / 2, (hi - lo).length() / 2);
    update();
}

bool ViewportItem::event(QEvent *e) {
    // While a modal transform runs, Escape, Backspace, digits and Delete belong to it, not to window shortcuts.
    if (e->type() == QEvent::ShortcutOverride && (modal_ || fly_ != Fly::none)) {
        e->accept();
        return true;
    }
    // Plain arrows, PgUp/PgDn and Home drive the camera here, not the window's shortcuts (Alt+arrows nudge).
    if (const auto *k = static_cast<QKeyEvent *>(e); e->type() == QEvent::ShortcutOverride && keys_ &&
                                                      !(k->modifiers() & Qt::AltModifier) &&
                                                      (k->key() >= Qt::Key_Home && k->key() <= Qt::Key_PageDown)) {
        e->accept();
        return true;
    }
    return QQuickRhiItem::event(e);
}

void ViewportItem::mousePressEvent(QMouseEvent *e) {
    last = press = e->position();
    dragged = false;
    press_hit.reset();
    e->accept();
    if (modal_) {
        finish(e->button() == Qt::LeftButton);
        return;
    }
    if (fly_ == Fly::walk) { // left keeps the view, right goes back
        if (e->button() == Qt::RightButton)
            camera = fly_start_;
        set_fly(Fly::none);
        return;
    }
    forceActiveFocus();
    const bool blender = Session::controls() != "maya";
    const auto mods = e->modifiers();
    if (e->button() == Qt::RightButton && !(mods & Qt::AltModifier)) {
        set_fly(Fly::hold);
        return;
    }
    nav_ = Nav::none;
    if (blender) {
        // Alt + left stands in for the middle button on trackpads (Blender's "emulate 3 button mouse").
        if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && mods & Qt::AltModifier))
            nav_ = mods & Qt::ShiftModifier ? Nav::pan : mods & Qt::ControlModifier ? Nav::zoom : Nav::orbit;
    } else if (mods & Qt::AltModifier) {
        nav_ = e->button() == Qt::LeftButton ? Nav::orbit : e->button() == Qt::MiddleButton ? Nav::pan : Nav::zoom;
    } else if (e->button() == Qt::MiddleButton)
        nav_ = Nav::pan;
    if (nav_ != Nav::none || e->button() != Qt::LeftButton || tool_ != "select")
        return;
    const auto [ro, rd] = ray_at(press);
    const sv::render::RenderScene scene = scene_now(gizmo_entry());
    press_hit = sv::render::pick(scene, ro, rd);
    if (!press_hit)
        return;
    if (press_hit->starts_with("gizmo:")) {
        const std::string handle = press_hit->substr(6);
        const std::string uuid = Session::instance()->current().value("uuid").toString().toStdString();
        if (start_edit(uuid, handle, scene.gizmo)) {
            const QVector3D fo = edit_->frame.column(3).toVector3D();
            if (handle == "ry" || handle == "s") {
                const auto hit = sv::render::plane_hit(ro, rd, fo, edit_->frame.column(1).toVector3D());
                const QVector3D v = hit.value_or(fo) - fo;
                edit_->t0 = handle == "s" ? v.length()
                                          : std::atan2(-QVector3D::dotProduct(v, edit_->frame.column(2).toVector3D()),
                                                       QVector3D::dotProduct(v, edit_->frame.column(0).toVector3D())) *
                                                180 / std::numbers::pi;
            } else
                edit_->t0 =
                    sv::render::axis_param(ro, rd, fo, edit_->frame.column(handle[1] - 'x').toVector3D()).value_or(0);
        }
    } else if (!(mods & (Qt::ShiftModifier | Qt::ControlModifier)) &&
               Session::instance()->selection().contains(*press_hit) && start_edit(*press_hit, "plane", std::nullopt))
        edit_->grab = sv::render::plane_hit(ro, rd, edit_->origin, {0, 1, 0}).value_or(edit_->origin);
}

void ViewportItem::mouseMoveEvent(QMouseEvent *e) {
    const QPointF d = e->position() - last;
    last = e->position();
    dragged |= (e->position() - press).manhattanLength() > 4;
    const bool free = e->modifiers() & Qt::AltModifier;
    if (modal_)
        drag_modal(last, free);
    else if (fly_ != Fly::none)
        camera.look(float(-d.x() * 0.2), float(-d.y() * 0.2));
    else if (edit_) {
        if (dragged)
            drag_handle(last, free);
    } else if (nav_ == Nav::pan)
        camera.pan(float(d.x() / height()), float(d.y() / height()));
    else if (nav_ == Nav::zoom)
        camera.zoom(float((d.x() - d.y()) / 20));
    else if (nav_ == Nav::orbit) {
        camera.orbit(float(-d.x() * 0.3), float(d.y() * 0.3));
        if (view_ != "Perspective")
            view_ = QStringLiteral("Perspective"), Q_EMIT viewChanged();
    }
    update();
}

void ViewportItem::hoverMoveEvent(QHoverEvent *e) {
    const QPointF d = e->position() - last;
    last = e->position();
    if (modal_)
        drag_modal(last, e->modifiers() & Qt::AltModifier);
    // shortcut: no cursor capture in walk mode, so looking stops at the window edge; warp with QCursor::setPos
    // when that bites.
    if (fly_ == Fly::walk)
        camera.look(float(-d.x() * 0.2), float(-d.y() * 0.2)), update();
}

void ViewportItem::mouseReleaseEvent(QMouseEvent *e) {
    Session *s = Session::instance();
    if (fly_ == Fly::hold && e->button() == Qt::RightButton) {
        set_fly(Fly::none);
        return;
    }
    if (nav_ != Nav::none) {
        nav_ = Nav::none;
        return;
    }
    if (edit_ && !modal_) {
        finish(dragged);
        if (dragged || press_hit->starts_with("gizmo:"))
            return;
    }
    if (dragged || e->button() != Qt::LeftButton)
        return;
    if (tool_ == "floor") {
        if (const QVariantList p = floorPoint(e->position().x(), e->position().y()); !p.isEmpty())
            Q_EMIT floorClicked(QVector3D(p[0].toFloat(), 0, p[2].toFloat()));
        return;
    }
    s->select(press_hit, e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier));
}

void ViewportItem::keyPressEvent(QKeyEvent *e) {
    const int k = e->key();
    const auto mods = e->modifiers() & ~Qt::KeypadModifier;
    if (modal_) {
        Edit &ed = *edit_;
        if (k == Qt::Key_Escape)
            finish(false);
        else if (k == Qt::Key_Return || k == Qt::Key_Enter)
            finish(true);
        else if (k == Qt::Key_X || k == Qt::Key_Y || k == Qt::Key_Z)
            ed.axis = ed.axis == k - Qt::Key_X ? -1 : k - Qt::Key_X;
        else if (k == Qt::Key_Backspace)
            ed.typed.chop(1);
        else if ((k >= Qt::Key_0 && k <= Qt::Key_9) || k == Qt::Key_Period || k == Qt::Key_Comma)
            ed.typed += k == Qt::Key_Comma ? QChar('.') : QChar(k);
        else if (k == Qt::Key_Minus)
            ed.typed = ed.typed.startsWith('-') ? ed.typed.mid(1) : "-" + ed.typed;
        if (edit_)
            drag_modal(last, false);
        e->accept();
        return;
    }
    if (!keys_) {
        e->ignore();
        return;
    }
    if (fly_ != Fly::none) {
        if (fly_ == Fly::walk && (k == Qt::Key_Escape || k == Qt::Key_Return || k == Qt::Key_Enter)) {
            if (k == Qt::Key_Escape)
                camera = fly_start_;
            set_fly(Fly::none);
        } else if (fly_ == Fly::walk && (k == Qt::Key_AsciiTilde || k == Qt::Key_QuoteLeft))
            set_fly(Fly::none);
        else if (!e->isAutoRepeat())
            held_.insert(k);
        e->accept();
        return;
    }
    Session *s = Session::instance();
    const bool blender = Session::controls() != "maya";
    const sv::app::ViewKey *key = sv::app::find_view_key(k, mods, e->modifiers() & Qt::KeypadModifier, blender,
                                                         Session::emulate_numpad());
    if (!key) {
        e->ignore();
        return;
    }
    e->accept();
    using A = sv::app::ViewAction;
    switch (key->action) {
    case A::orbit:
        camera.orbit(key->x, key->y);
        if (view_ != "Perspective")
            view_ = QStringLiteral("Perspective"), Q_EMIT viewChanged();
        break;
    case A::pan:
        camera.pan(key->x, key->y);
        break;
    case A::zoom:
        camera.zoom(key->x);
        break;
    case A::preset:
        return setPreset(QString::fromUtf8(key->name));
    case A::flip:
        camera.flip();
        if (view_ != "Perspective")
            view_ = QStringLiteral("Perspective"), Q_EMIT viewChanged();
        break;
    case A::frame:
    case A::frame_all:
        return frameSelection(key->action == A::frame_all);
    case A::walk:
        return set_fly(Fly::walk);
    case A::transform:
        beginTransform(QString::fromUtf8(key->name));
        return;
    case A::remove:
        return s->deleteSelection();
    case A::duplicate:
        s->duplicateSelection();
        beginTransform("move");
        return;
    case A::select_all: {
        QStringList all;
        for (const json &o : s->document().data().at("objects"))
            all << QString::fromStdString(o.value("uuid", std::string()));
        return s->selectUuids(all);
    }
    case A::select_none:
        return s->selectUuid({}, false);
    case A::gizmo:
        return setGizmoMode(QString::fromUtf8(key->name));
    }
    update();
}

void ViewportItem::keyReleaseEvent(QKeyEvent *e) {
    if (!e->isAutoRepeat())
        held_.erase(e->key());
    e->setAccepted(fly_ != Fly::none);
}

void ViewportItem::set_fly(Fly fly) {
    if (fly == fly_)
        return;
    if (fly != Fly::none && fly_ == Fly::none)
        fly_start_ = camera, held_.clear(), fly_clock_.start(), forceActiveFocus();
    fly_ = fly;
    if (fly != Fly::none && view_ != "Perspective")
        view_ = QStringLiteral("Perspective"), Q_EMIT viewChanged();
    Q_EMIT flyingChanged();
    update();
}

void ViewportItem::step_fly() {
    const float dt = std::min(float(fly_clock_.restart()) / 1000, 0.1f); // a stalled frame does not jump
    QVector3D move;
    for (int k : held_)
        move += k == Qt::Key_W   ? QVector3D(0, 0, 1)
                : k == Qt::Key_S ? QVector3D(0, 0, -1)
                : k == Qt::Key_A ? QVector3D(-1, 0, 0)
                : k == Qt::Key_D ? QVector3D(1, 0, 0)
                : k == Qt::Key_E ? QVector3D(0, 1, 0)
                : k == Qt::Key_Q ? QVector3D(0, -1, 0)
                                 : QVector3D();
    const auto mods = QGuiApplication::queryKeyboardModifiers();
    const float boost = mods & Qt::ShiftModifier ? 4 : mods & Qt::AltModifier ? 0.25f : 1;
    if (!move.isNull())
        camera.fly(move.normalized() * fly_speed_ * boost * dt);
    update();
}

QVariantList ViewportItem::keyHelp() const { return sv::app::view_key_help(Session::controls() != "maya"); }

void ViewportItem::setPreset(const QString &name) {
    using P = sv::render::OrbitCamera::Preset;
    static const std::map<QString, P> presets{
        {"Perspective", P::perspective}, {"Top", P::top}, {"Front", P::front}, {"Side", P::side},
        {"Back", P::back},           {"Other side", P::left}, {"Bottom", P::bottom}};
    if (const auto it = presets.find(name); it != presets.end()) {
        camera.set_preset(it->second);
        view_ = name;
        Q_EMIT viewChanged();
        update();
    }
}

QVariantMap ViewportItem::cameraState() const {
    return {{"target", QVariantList{camera.target.x(), camera.target.y(), camera.target.z()}},
            {"yaw", camera.yaw},
            {"pitch", camera.pitch},
            {"distance", camera.distance},
            {"fov", camera.fov},
            {"view", view_}};
}

void ViewportItem::setCameraState(const QVariantMap &state) {
    const QVariantList t = state.value("target").toList();
    if (t.size() == 3)
        camera.target = QVector3D(t[0].toFloat(), t[1].toFloat(), t[2].toFloat());
    camera.yaw = state.value("yaw", camera.yaw).toFloat();
    camera.pitch = state.value("pitch", camera.pitch).toFloat();
    camera.distance = state.value("distance", camera.distance).toFloat();
    camera.fov = state.value("fov", camera.fov).toFloat();
    view_ = state.value("view", view_).toString();
    Q_EMIT viewChanged();
    update();
}

void ViewportItem::wheelEvent(QWheelEvent *e) {
    if (fly_ != Fly::none) {
        fly_speed_ = std::clamp(fly_speed_ * std::pow(1.25f, float(e->angleDelta().y()) / 120), 0.25f, 100.0f);
        return;
    }
    camera.zoom(float(e->angleDelta().y()) / 120);
    update();
}
