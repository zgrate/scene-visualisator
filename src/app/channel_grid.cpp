#include "channel_grid.h"

#include "protocol/merger.h"
#include "session.h"

#include <QPainter>

ChannelGrid::ChannelGrid(QQuickItem *parent) : QQuickPaintedItem(parent) {
    if (Session *s = Session::instance()) {
        connect(s, &Session::dmxChanged, this, &ChannelGrid::pull);
        connect(s, &Session::changed, this, &ChannelGrid::pull);
    }
    connect(this, &ChannelGrid::universeChanged, this, &ChannelGrid::pull);
    connect(this, &ChannelGrid::styleChanged, this, [this] { update(); });
}

void ChannelGrid::pull() {
    std::array<std::uint8_t, 512> values{}, winner;
    winner.fill(sv::proto::UniverseMerger::no_winner);
    for (const auto &st : Session::instance()->status())
        if (st.universe == universe_)
            values = st.data, winner = st.winner;
    const auto occupancy = Session::instance()->occupancy(universe_);
    if (values != values_ || winner != winner_ || occupancy != occupancy_) {
        values_ = values, winner_ = winner, occupancy_ = occupancy;
        update();
    }
}

void ChannelGrid::paint(QPainter *p) {
    constexpr int cols = 32, rows = 16;
    const qreal w = width() / cols, h = height() / rows;
    QFont small = font_;
    small.setPixelSize(std::max(7, font_.pixelSize() - 3));
    for (int i = 0; i < 512; ++i) {
        const QRectF cell(i % cols * w, i / cols * h, w - 1, h - 1);
        const std::uint8_t v = values_[std::size_t(i)], occ = occupancy_[std::size_t(i)];
        p->fillRect(cell, QColor(line_.red(), line_.green(), line_.blue(), occ ? 110 : 45));
        if (v) {
            QColor f = fill_;
            f.setAlphaF(0.55f);
            const qreal fh = cell.height() * v / 255;
            p->fillRect(QRectF(cell.left(), cell.bottom() - fh, cell.width(), fh), f);
        }
        if (const auto win = winner_[std::size_t(i)];
            win != sv::proto::UniverseMerger::no_winner && !source_colours_.isEmpty())
            p->fillRect(QRectF(cell.left(), cell.bottom() - 2, cell.width(), 2),
                        source_colours_[win % source_colours_.size()].value<QColor>());
        if (occ >= 2) {
            p->setPen(QPen(occ == 3 ? highlight_ : conflict_, 1.5));
            p->drawRect(cell.adjusted(0.75, 0.75, -0.75, -0.75));
        }
        p->setPen(muted_);
        p->setFont(small);
        p->drawText(cell.adjusted(2, 1, 0, 0), Qt::AlignLeft | Qt::AlignTop, QString::number(i + 1));
        p->setPen(v ? text_ : muted_);
        p->setFont(font_);
        p->drawText(cell.adjusted(0, 0, -3, -2), Qt::AlignRight | Qt::AlignBottom, QString::number(v));
    }
}
