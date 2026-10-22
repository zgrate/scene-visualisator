#include "drawing_item.h"

#include "render/drawing_painter.h"
#include "session.h"

#include <QPainter>
#include <cmath>

DrawingItem::DrawingItem(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton);
    if (Session *s = Session::instance())
        connect(s, &Session::changed, this, [this] { update(); });
    connect(this, &DrawingItem::kindChanged, this, [this] { update(); });
}

void DrawingItem::paint(QPainter *p) {
    Session *s = Session::instance();
    if (!s)
        return;
    const sv::render::DrawingSheet sheet = s->drawing_sheet(kind_);
    const QSizeF mm = sv::render::sheet_size_mm(sheet);
    const double k = std::min((width() - 24) / mm.width(), (height() - 24) / mm.height());
    if (k <= 0)
        return;
    const QTransform page = QTransform::fromTranslate((width() - mm.width() * k) / 2, (height() - mm.height() * k) / 2)
                                .scale(k, k); // page millimetres -> item pixels
    p->setTransform(page);
    to_item_ = sv::render::paint_drawing(*p, *s->snapshot(), *s->library_snapshot(), *s->meshes(), sheet) * page;
    if (dragging_) {
        p->resetTransform();
        p->setPen(QPen(QColor(0x1f, 0x5f, 0xd6), 1.5, Qt::DashLine));
        p->drawLine(drag_from_, drag_to_);
    }
}

void DrawingItem::mousePressEvent(QMouseEvent *e) {
    if (!(e->modifiers() & Qt::ShiftModifier)) {
        e->ignore();
        return;
    }
    dragging_ = true;
    drag_from_ = drag_to_ = e->position();
    update();
}

void DrawingItem::mouseMoveEvent(QMouseEvent *e) {
    if (!dragging_)
        return;
    drag_to_ = e->position();
    update();
}

void DrawingItem::mouseReleaseEvent(QMouseEvent *e) {
    if (!dragging_)
        return;
    dragging_ = false;
    update();
    bool ok = false;
    const QTransform back = to_item_.inverted(&ok);
    if (!ok || QLineF(drag_from_, e->position()).length() < 4)
        return;
    const auto snap = [](double m) { return std::round(m * 20) / 20; }; // 5 cm
    const QPointF a = back.map(drag_from_), b = back.map(e->position());
    QVariantList dims = Session::instance()->drawing(kind_).value(QStringLiteral("dimensions")).toList();
    dims.append(QVariantMap{{QStringLiteral("a"), QVariantList{snap(a.x()), snap(a.y())}},
                            {QStringLiteral("b"), QVariantList{snap(b.x()), snap(b.y())}}});
    Session::instance()->setDrawing(kind_, QStringLiteral("dimensions"), dims);
}
