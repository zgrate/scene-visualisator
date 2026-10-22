#include "universe_meter.h"

#include "session.h"

#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>
#include <algorithm>

UniverseMeter::UniverseMeter(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(ItemHasContents);
    if (Session *s = Session::instance())
        connect(s, &Session::dmxChanged, this, &UniverseMeter::pull);
    connect(this, &UniverseMeter::universeChanged, this, &UniverseMeter::pull);
    connect(this, &UniverseMeter::colorChanged, this, &QQuickItem::update);
}

void UniverseMeter::pull() {
    std::array<std::uint8_t, 512> next{};
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
        auto *g = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 512 * 6);
        g->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(g);
        node->setFlag(QSGNode::OwnsGeometry);
        node->setMaterial(new QSGFlatColorMaterial);
        node->setFlag(QSGNode::OwnsMaterial);
    }
    static_cast<QSGFlatColorMaterial *>(node->material())->setColor(color_);
    const float w = float(width()) / 512, h = float(height());
    const float gap = w > 2 ? 1 : 0;
    QSGGeometry::Point2D *v = node->geometry()->vertexDataAsPoint2D();
    for (int i = 0; i < 512; ++i, v += 6) {
        // Zero still shows a 1 px stub so the channel grid stays readable.
        const float x0 = i * w, x1 = x0 + w - gap, y1 = h, y0 = h - std::max(1.f, h * levels_[i] / 255.f);
        v[0].set(x0, y0), v[1].set(x1, y0), v[2].set(x0, y1);
        v[3].set(x1, y0), v[4].set(x1, y1), v[5].set(x0, y1);
    }
    node->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
    return node;
}
