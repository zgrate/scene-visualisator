#include "viewport_item.h"

#include "render/scene_renderer.h"
#include "render/test_pattern.h"
#include "session.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <cmath>
#include <map>

namespace {

class ViewportRenderer : public QQuickRhiItemRenderer {
    void initialize(QRhiCommandBuffer *) override { scene.initialize(rhi(), renderTarget()); }

    void synchronize(QQuickRhiItem *item) override {
        auto *view = static_cast<ViewportItem *>(item);
        Session *s = Session::instance();
        scene.set_camera(view->camera, double(view->clock.elapsed()) / 1000);
        if (s->revision() != revision) {
            revision = s->revision();
            project = s->snapshot();
            library = s->library_snapshot();
            meshes = s->meshes();
            selection = s->selection();
            scene.set_composition(s->composition());
            composition_size = s->composition().size();
            media_serial = 0; // a live source's current frame goes back over the redrawn composition
            buffers.clear();
            for (const QVariant &u : s->universes()) // every universe a fixture or strip reads
                buffers.try_emplace(std::uint16_t(u.toInt()), s->dmx().frames(std::uint16_t(u.toInt())));
        }
        time = double(view->clock.elapsed()) / 1000;
    }

    void render(QRhiCommandBuffer *cb) override {
        if (QImage frame = Session::instance()->media().latest(media_serial); !frame.isNull())
            // Placed by source.fit unless it already matches the composition. shortcut: a CPU blit per frame;
            // scale in the wall shader when 4K sources make it show up in profiles (NFR-2).
            scene.set_composition(project && frame.size() != composition_size
                                      ? sv::render::make_composition(*project, frame)
                                      : std::move(frame));
        if (project) {
            for (auto &[u, b] : buffers)
                b->update();
            // shortcut: decodes and rebuilds every fixture each frame; cache per fixture when rigs reach 1000 (NFR-2).
            const auto fixtures = sv::render::decode_fixtures(*project, *library, [&](std::uint16_t u) {
                auto it = buffers.find(u);
                return it == buffers.end() ? std::span<const std::uint8_t>()
                                           : std::span<const std::uint8_t>(it->second->front().data);
            });
            scene.set_scene(sv::render::build_scene(*project, fixtures, selection, time, *meshes));
        }
        scene.render(cb, renderTarget());
    }

    sv::render::SceneRenderer scene;
    quint64 revision = ~0ull;
    quint64 media_serial = 0;
    QSize composition_size;
    std::shared_ptr<const sv::json> project;
    std::shared_ptr<const sv::render::FixtureLibrary> library;
    std::shared_ptr<const sv::render::MeshLibrary> meshes;
    std::set<std::string> selection;
    std::map<std::uint16_t, sv::proto::TripleBuffer<sv::proto::UniverseFrame> *> buffers;
    double time = 0;
};

} // namespace

ViewportItem::ViewportItem(QQuickItem *parent) : QQuickRhiItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton);
    setSampleCount(4);
    clock.start();
    // shortcut: redraws at a fixed 60 Hz for live DMX; only redraw when a frame or the document changes (NFR-2 work).
    tick.setInterval(16);
    connect(&tick, &QTimer::timeout, this, &QQuickItem::update);
    tick.start();
}

QQuickRhiItemRenderer *ViewportItem::createRenderer() { return new ViewportRenderer; }

std::optional<QVector3D> ViewportItem::on_plane(QPointF p, float y) const {
    const auto [origin, dir] = camera.ray(float(p.x() / width()), float(p.y() / height()), float(width() / height()));
    if (std::abs(dir.y()) < 1e-4f)
        return std::nullopt;
    const float t = (y - origin.y()) / dir.y();
    return t > 0 ? std::optional(origin + dir * t) : std::nullopt;
}

namespace {
std::optional<std::string> pick_at(const sv::render::OrbitCamera &camera, QPointF p, QSizeF size) {
    Session *s = Session::instance();
    const auto [origin, dir] =
        camera.ray(float(p.x() / size.width()), float(p.y() / size.height()), float(size.width() / size.height()));
    const auto blank = [](std::uint16_t) { return std::span<const std::uint8_t>(); };
    const auto fixtures = sv::render::decode_fixtures(*s->snapshot(), s->library(), blank);
    return sv::render::pick(sv::render::build_scene(*s->snapshot(), fixtures, {}, 0, *s->meshes()), origin, dir);
}
} // namespace

void ViewportItem::mousePressEvent(QMouseEvent *e) {
    last = press = e->position();
    dragged = false;
    grab.reset();
    Session *s = Session::instance();
    if (e->button() == Qt::LeftButton && !(e->modifiers() & Qt::ShiftModifier))
        if (auto hit = pick_at(camera, press, size()); hit && s->selection().contains(*hit))
            for (const sv::json &o : s->document().data().at("objects"))
                if (o.value("uuid", std::string()) == *hit) {
                    const auto &v = o.at("position");
                    const QVector3D pos(v[0].get<float>(), v[1].get<float>(), v[2].get<float>());
                    if (const auto at = on_plane(press, pos.y()))
                        grab = Grab{*hit, pos, *at - pos};
                }
    e->accept();
}

void ViewportItem::mouseMoveEvent(QMouseEvent *e) {
    const QPointF d = e->position() - last;
    last = e->position();
    dragged |= (e->position() - press).manhattanLength() > 4;
    if (grab) {
        if (!dragged)
            return;
        if (const auto at = on_plane(e->position(), grab->start.y())) {
            QVector3D p = *at - grab->offset;
            if (!(e->modifiers() & Qt::AltModifier))
                p = QVector3D(std::round(p.x() * 4) / 4, p.y(), std::round(p.z() * 4) / 4);
            Session::instance()->preview_move(grab->uuid, p);
        }
    } else if (e->buttons() & Qt::MiddleButton || e->modifiers() & Qt::ShiftModifier)
        camera.pan(float(d.x() / height()), float(d.y() / height()));
    else {
        camera.orbit(float(-d.x() * 0.3), float(d.y() * 0.3));
        if (view_ != "Perspective")
            view_ = QStringLiteral("Perspective"), Q_EMIT viewChanged();
    }
    update();
}

void ViewportItem::mouseReleaseEvent(QMouseEvent *e) {
    Session *s = Session::instance();
    if (grab && dragged) {
        s->commit_move();
        grab.reset();
        return;
    }
    grab.reset();
    if (dragged || e->button() != Qt::LeftButton)
        return;
    s->select(pick_at(camera, e->position(), size()), e->modifiers() & Qt::ControlModifier);
}

void ViewportItem::setPreset(const QString &name) {
    using P = sv::render::OrbitCamera::Preset;
    static const std::map<QString, P> presets{
        {"Perspective", P::perspective}, {"Top", P::top}, {"Front", P::front}, {"Side", P::side}};
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
    camera.zoom(float(e->angleDelta().y()) / 120);
    update();
}
