#include "universe_meter.h"

#include "session.h"

#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>
#include <algorithm>

UniverseMeter::UniverseMeter(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(ItemHasContents);
    if (Session *s = Session::instance()) {
        connect(s, &Session::dmxChanged, this, [this] {
            if (!occupancy_)
                pull();
        });
        connect(s, &Session::changed, this, [this] {
            if (occupancy_)
                pull();
        });
    }
    connect(this, &UniverseMeter::universeChanged, this, &UniverseMeter::pull);
    connect(this, &UniverseMeter::colorChanged, this, &QQuickItem::update);
}

void UniverseMeter::pull() {
    std::array<std::uint8_t, 512> next{};
    if (occupancy_)
        next = Session::instance()->occupancy(universe_);
    else
        for (const auto &st : Session::instance()->status())
            if (st.universe == universe_)
                next = st.data;
    if (next != levels_) {
        levels_ = next;
        update();
    }
}

QSGNode *UniverseMeter::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
    auto *node = static_cast<QSGGeometryNode *>(old);
    if (!node) {
        node = new QSGGeometryNode;
        auto *g = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 512 * 6);
        g->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(g);
        node->setFlag(QSGNode::OwnsGeometry);
        node->setMaterial(new QSGVertexColorMaterial);
        node->setFlag(QSGNode::OwnsMaterial);
    }
    const float w = float(width()) / 512, h = float(height());
    const float gap = w > 3 ? 1 : 0;
    const QColor colours[] = {color_, color_, conflict_, highlight_};
    QSGGeometry::ColoredPoint2D *v = node->geometry()->vertexDataAsColoredPoint2D();
    for (int i = 0; i < 512; ++i, v += 6) {
        const std::uint8_t l = levels_[std::size_t(i)];
        const float level = occupancy_ ? (l ? 1.f : 0.f) : l / 255.f;
        // Zero still shows a 1 px stub so the channel grid stays readable.
        const float x0 = i * w, x1 = x0 + w - gap, y1 = h, y0 = h - std::max(1.f, h * level);
        // The vertex colour material expects premultiplied alpha.
        const QColor c = occupancy_ ? colours[std::min<int>(l, 3)] : color_;
        const auto r = uchar(c.red() * c.alphaF()), gr = uchar(c.green() * c.alphaF()),
                   b = uchar(c.blue() * c.alphaF()), a = uchar(c.alpha());
        v[0].set(x0, y0, r, gr, b, a), v[1].set(x1, y0, r, gr, b, a), v[2].set(x0, y1, r, gr, b, a);
        v[3].set(x1, y0, r, gr, b, a), v[4].set(x1, y1, r, gr, b, a), v[5].set(x0, y1, r, gr, b, a);
    }
    node->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
    return node;
}
