#include "viewport_item.h"

#include "render/scene_renderer.h"
#include "session.h"

#include <QGuiApplication>
#include <QMouseEvent>
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
            library = &s->library();
            selection = s->selection();
            scene.set_composition(s->composition());
            buffers.clear();
            for (const sv::json &o : project->value("objects", sv::json::array()))
                if (const sv::json &p = o.value("patch", sv::json()); p.is_object()) {
                    const auto u = p.value("universe", std::uint16_t(1));
                    buffers.try_emplace(u, s->dmx().frames(u));
                }
        }
        time = double(view->clock.elapsed()) / 1000;
    }

    void render(QRhiCommandBuffer *cb) override {
        if (project) {
            for (auto &[u, b] : buffers)
                b->update();
            // shortcut: decodes and rebuilds every fixture each frame; cache per fixture when rigs reach 1000 (NFR-2).
            const auto fixtures = sv::render::decode_fixtures(*project, *library, [&](std::uint16_t u) {
                auto it = buffers.find(u);
                return it == buffers.end() ? std::span<const std::uint8_t>()
                                           : std::span<const std::uint8_t>(it->second->front().data);
            });
            scene.set_scene(sv::render::build_scene(*project, fixtures, selection, time));
        }
        scene.render(cb, renderTarget());
    }

    sv::render::SceneRenderer scene;
    quint64 revision = ~0ull;
    std::shared_ptr<const sv::json> project;
    const sv::render::FixtureLibrary *library = nullptr;
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

void ViewportItem::mousePressEvent(QMouseEvent *e) {
    last = press = e->position();
    dragged = false;
    e->accept();
}

void ViewportItem::mouseMoveEvent(QMouseEvent *e) {
    const QPointF d = e->position() - last;
    last = e->position();
    dragged |= (e->position() - press).manhattanLength() > 4;
    if (e->buttons() & Qt::MiddleButton || e->modifiers() & Qt::ShiftModifier)
        camera.pan(float(d.x() / height()), float(d.y() / height()));
    else
        camera.orbit(float(-d.x() * 0.3), float(d.y() * 0.3));
    update();
}

void ViewportItem::mouseReleaseEvent(QMouseEvent *e) {
    if (dragged || e->button() != Qt::LeftButton)
        return;
    Session *s = Session::instance();
    const auto [origin, dir] =
        camera.ray(float(e->position().x() / width()), float(e->position().y() / height()), float(width() / height()));
    const auto blank = [](std::uint16_t) { return std::span<const std::uint8_t>(); };
    const auto fixtures = sv::render::decode_fixtures(*s->snapshot(), s->library(), blank);
    s->select(sv::render::pick(sv::render::build_scene(*s->snapshot(), fixtures, {}, 0), origin, dir),
              e->modifiers() & Qt::ControlModifier);
}

void ViewportItem::wheelEvent(QWheelEvent *e) {
    camera.zoom(float(e->angleDelta().y()) / 120);
    update();
}
