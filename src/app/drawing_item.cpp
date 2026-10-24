#include "drawing_item.h"

#include "session.h"

#include <QPainter>
#include <cmath>

namespace {

const QColor highlight(0x1f, 0x5f, 0xd6); // the sheet's own screen blue, not a Theme colour: it sits on the paper

double snap5(double m) { return std::round(m * 20) / 20; } // 5 cm

QPointF point_of(const sv::json &a) { return {a[0].get<double>(), a[1].get<double>()}; }
sv::json json_of(QPointF p) { return sv::json::array({p.x(), p.y()}); }

bool valid_dimension(const sv::json &d) {
    return d.is_object() && d.contains("a") && d.contains("b") && d["a"].size() == 2 && d["b"].size() == 2;
}

double distance_to_segment(QPointF p, QPointF a, QPointF b) {
    const QPointF d = b - a;
    const double len2 = QPointF::dotProduct(d, d);
    const double t = len2 > 0 ? std::clamp(QPointF::dotProduct(p - a, d) / len2, 0.0, 1.0) : 0;
    return QLineF(p, a + d * t).length();
}

} // namespace

DrawingItem::DrawingItem(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton);
    setActiveFocusOnTab(true);
    if (Session *s = Session::instance())
        connect(s, &Session::changed, this, [this, s] {
            // an undo or another screen may have removed the picked dimension or note
            const sv::render::DrawingSheet sheet = s->drawing_sheet(kind_);
            if (dimension_ >= int(sheet.dimensions.size()) || note_ >= int(sheet.notes.size())) {
                select_sheet(-1, -1);
                if (drag_ != Drag::objects && drag_ != Drag::pan)
                    drag_ = Drag::none; // a sheet drag must not index the removed entry
            }
            update();
        });
    connect(this, &DrawingItem::kindChanged, this, [this] {
        select_sheet(-1, -1);
        update();
    });
}

void DrawingItem::select_sheet(int dimension, int note) {
    if (dimension == dimension_ && note == note_)
        return;
    dimension_ = dimension;
    note_ = note;
    Q_EMIT sheetSelectionChanged();
    update();
}

void DrawingItem::fit() {
    zoom_ = 1;
    pan_ = {};
    Q_EMIT viewChanged();
    update();
}

void DrawingItem::paint(QPainter *p) {
    Session *s = Session::instance();
    if (!s)
        return;
    sv::render::DrawingSheet sheet = s->drawing_sheet(kind_);
    sheet.selected = s->selection();
    if (drag_ == Drag::dimension_end || drag_ == Drag::dimension_offset)
        sheet.dimensions = preview_;
    else if (drag_ == Drag::note)
        sheet.notes = preview_;
    const QSizeF mm = sv::render::sheet_size_mm(sheet);
    const double k = std::min((width() - 24) / mm.width(), (height() - 24) / mm.height());
    if (k <= 0)
        return;
    page_ = QTransform::fromTranslate((width() - mm.width() * k) / 2, (height() - mm.height() * k) / 2)
                .scale(k, k) *                                  // page millimetres -> fitted item pixels
            QTransform(zoom_, 0, 0, zoom_, pan_.x(), pan_.y()); // then the on-screen zoom and pan
    p->setTransform(page_);
    std::vector<sv::render::DrawingHit> hits;
    to_item_ =
        sv::render::paint_drawing(*p, *s->snapshot(), *s->library_snapshot(), *s->meshes(), sheet, &hits) * page_;
    hits_.clear();
    for (const sv::render::DrawingHit &h : hits)
        hits_.push_back({h.uuid, page_.mapRect(h.page)});

    // Overlays in item pixels.
    p->resetTransform();
    p->setRenderHint(QPainter::Antialiasing);
    if (dimension_ >= 0 && dimension_ < int(sheet.dimensions.size()) && valid_dimension(sheet.dimensions[dimension_])) {
        const sv::json &d = sheet.dimensions[dimension_];
        const QPointF a = to_item_.map(point_of(d["a"])), b = to_item_.map(point_of(d["b"]));
        const double len = QLineF(a, b).length();
        if (len > 0) {
            const QPointF n = QPointF(b.y() - a.y(), a.x() - b.x()) / len * (d.value("offset", 5.0) * page_.m11());
            p->setPen(QPen(highlight, 2));
            p->drawLine(a + n, b + n);
            p->setBrush(Qt::white);
            p->drawEllipse(a, 4, 4);
            p->drawEllipse(b, 4, 4);
            p->setBrush(Qt::NoBrush);
        }
    }
    if (note_ >= 0)
        for (const auto &h : hits_)
            if (h.uuid == "note:" + std::to_string(note_)) {
                p->setPen(QPen(highlight, 1, Qt::DashLine));
                p->drawRect(h.page.adjusted(-2, -2, 2, 2));
            }
    p->setPen(QPen(highlight, 1.5, Qt::DashLine));
    if (drag_ == Drag::new_dimension || drag_ == Drag::new_note)
        p->drawLine(press_, cur_);
    if (drag_ == Drag::objects && QLineF(press_, cur_).length() >= 4) {
        const QVector3D w = world_delta(cur_, false);
        const QPointF d = to_item_.map(sv::render::drawing_point(kind_.toStdString(), w)) - to_item_.map(QPointF());
        for (const auto &h : hits_)
            if (sheet.selected.contains(h.uuid))
                p->drawRect(h.page.translated(d));
    }
}

QPointF DrawingItem::to_drawing(QPointF item) const { return to_item_.inverted().map(item); }

std::string DrawingItem::hit(QPointF item) const {
    const sv::render::DrawingHit *best = nullptr;
    for (const auto &h : hits_)
        if (!h.uuid.starts_with("note:") && h.page.contains(item) &&
            (!best || h.page.width() * h.page.height() < best->page.width() * best->page.height()))
            best = &h;
    return best ? best->uuid : std::string();
}

// The drag from the press to `item` as a world displacement, snapped to 5 cm per axis.
QVector3D DrawingItem::world_delta(QPointF item, bool free) const {
    QVector3D w = sv::render::drawing_delta(kind_.toStdString(), to_drawing(item) - to_drawing(press_));
    if (!free)
        w = QVector3D(snap5(w.x()), snap5(w.y()), snap5(w.z()));
    return w;
}

void DrawingItem::write(const char *key, const sv::json &list) {
    Session::instance()->setDrawing(kind_, QString::fromLatin1(key), Session::to_variant_list(list));
}

void DrawingItem::mousePressEvent(QMouseEvent *e) {
    Session *s = Session::instance();
    if (!s)
        return;
    forceActiveFocus(Qt::MouseFocusReason);
    press_ = cur_ = e->position();
    const auto mods = e->modifiers();
    if (e->button() == Qt::MiddleButton) {
        drag_ = Drag::pan;
        return;
    }
    const sv::render::DrawingSheet sheet = s->drawing_sheet(kind_);
    // 1. A dimension end handle, the picked dimension first.
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < int(sheet.dimensions.size()); ++i) {
            const sv::json &d = sheet.dimensions[i];
            if ((pass == 0) != (i == dimension_) || !valid_dimension(d))
                continue;
            for (int end = 0; end < 2; ++end)
                if (QLineF(press_, to_item_.map(point_of(d[end ? "b" : "a"]))).length() <= 6) {
                    select_sheet(i, -1);
                    drag_ = Drag::dimension_end;
                    end_ = end;
                    preview_ = sheet.dimensions;
                    return;
                }
        }
    // 2. A dimension line (offset from the measured points).
    for (int i = 0; i < int(sheet.dimensions.size()); ++i) {
        const sv::json &d = sheet.dimensions[i];
        if (!valid_dimension(d))
            continue;
        const QPointF a = to_item_.map(point_of(d["a"])), b = to_item_.map(point_of(d["b"]));
        const double len = QLineF(a, b).length();
        if (len <= 0)
            continue;
        const QPointF n = QPointF(b.y() - a.y(), a.x() - b.x()) / len * (d.value("offset", 5.0) * page_.m11());
        if (distance_to_segment(press_, a + n, b + n) <= 5) {
            select_sheet(i, -1);
            drag_ = Drag::dimension_offset;
            preview_ = sheet.dimensions;
            return;
        }
    }
    // 3. A note's text.
    for (const auto &h : hits_)
        if (h.uuid.starts_with("note:") && h.page.contains(press_)) {
            select_sheet(-1, std::stoi(h.uuid.substr(5)));
            drag_ = Drag::note;
            preview_ = sheet.notes;
            return;
        }
    select_sheet(-1, -1);
    // 4. A rig object.
    if (const std::string uuid = hit(press_); !uuid.empty() && tool_ != QStringLiteral("note")) {
        const bool add = mods & (Qt::ShiftModifier | Qt::ControlModifier);
        if (add || !s->selection().contains(uuid))
            s->selectUuid(QString::fromStdString(uuid), add);
        grabbed_ = uuid;
        drag_ = s->selection().contains(uuid) ? Drag::objects : Drag::none;
        return;
    }
    if (mods & Qt::ShiftModifier) // 5. a new dimension
        drag_ = Drag::new_dimension;
    else if (tool_ == QStringLiteral("note")) // 6. a new note
        drag_ = Drag::new_note;
    else if (mods & Qt::AltModifier)
        drag_ = Drag::pan;
    else // 7. empty paper
        s->selectUuid(QString(), false);
}

void DrawingItem::mouseMoveEvent(QMouseEvent *e) {
    if (drag_ == Drag::none)
        return;
    const QPointF last = cur_;
    cur_ = e->position();
    switch (drag_) {
    case Drag::pan:
        pan_ += cur_ - last;
        Q_EMIT viewChanged();
        break;
    case Drag::dimension_end: {
        const QPointF m = to_drawing(cur_);
        preview_[dimension_][end_ ? "b" : "a"] = json_of(QPointF(snap5(m.x()), snap5(m.y())));
        break;
    }
    case Drag::dimension_offset: {
        const sv::json &d = preview_[dimension_];
        const QPointF a = to_item_.map(point_of(d["a"])), b = to_item_.map(point_of(d["b"]));
        const double len = QLineF(a, b).length();
        const QPointF n(QPointF(b.y() - a.y(), a.x() - b.x()) / len);
        preview_[dimension_]["offset"] = std::round(QPointF::dotProduct(cur_ - a, n) / page_.m11() * 2) / 2; // 0.5 mm
        break;
    }
    case Drag::note: {
        const sv::json &before = Session::instance()->drawing_sheet(kind_).notes[note_];
        preview_[note_]["at"] = json_of(point_of(before["at"]) + to_drawing(cur_) - to_drawing(press_));
        break;
    }
    default:
        break;
    }
    update();
}

void DrawingItem::mouseReleaseEvent(QMouseEvent *e) {
    Session *s = Session::instance();
    const Drag drag = std::exchange(drag_, Drag::none);
    update();
    if (!s)
        return;
    cur_ = e->position();
    const bool moved = QLineF(press_, cur_).length() >= 4;
    switch (drag) {
    case Drag::dimension_end:
    case Drag::dimension_offset:
        if (moved)
            write("dimensions", preview_);
        break;
    case Drag::note:
        if (moved)
            write("notes", preview_);
        break;
    case Drag::new_dimension:
        if (moved) {
            const QPointF a = to_drawing(press_), b = to_drawing(cur_);
            sv::json dims = s->drawing_sheet(kind_).dimensions;
            dims.push_back({{"a", json_of(QPointF(snap5(a.x()), snap5(a.y())))},
                            {"b", json_of(QPointF(snap5(b.x()), snap5(b.y())))}});
            write("dimensions", dims);
            select_sheet(int(dims.size()) - 1, -1);
        }
        break;
    case Drag::new_note: {
        sv::json note = {{"at", json_of(to_drawing(cur_))}, {"text", ""}};
        if (moved)
            note["to"] = json_of(to_drawing(press_));
        tool_ = QStringLiteral("select");
        Q_EMIT toolChanged();
        Q_EMIT editNote(-1, Session::to_variant_list(sv::json::array({note}))[0].toMap(), cur_);
        break;
    }
    case Drag::objects: {
        if (!moved) { // a plain click on one of several selected objects picks it alone
            if (!(e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)) && s->selection().size() > 1)
                s->selectUuid(QString::fromStdString(grabbed_), false);
            break;
        }
        QVector3D w = world_delta(cur_, e->modifiers() & Qt::AltModifier);
        // shortcut: one parent frame for the whole selection, per-object frames if mixed-parent drags come up
        const sv::json &project = *s->snapshot();
        const sv::json *grabbed = nullptr, *parent = nullptr;
        if (project.contains("objects"))
            for (const sv::json &o : project.at("objects"))
                if (o.value("uuid", std::string()) == grabbed_)
                    grabbed = &o;
        if (grabbed && grabbed->contains("parent") && (*grabbed)["parent"].is_string())
            for (const sv::json &o : project.at("objects"))
                if (o.value("uuid", std::string()) == (*grabbed)["parent"].get<std::string>())
                    parent = &o;
        if (parent)
            w = sv::render::world_transform(project, *parent).inverted().mapVector(w);
        s->nudgeSelection(w.x(), w.y(), w.z());
        break;
    }
    default:
        break;
    }
}

void DrawingItem::mouseDoubleClickEvent(QMouseEvent *e) {
    for (const auto &h : hits_)
        if (h.uuid.starts_with("note:") && h.page.contains(e->position())) {
            const int i = std::stoi(h.uuid.substr(5));
            const sv::json notes = Session::instance()->drawing_sheet(kind_).notes;
            if (i < int(notes.size()))
                Q_EMIT editNote(i, Session::to_variant_list(sv::json::array({notes[i]}))[0].toMap(), e->position());
            return;
        }
    QQuickPaintedItem::mouseDoubleClickEvent(e);
}

void DrawingItem::wheelEvent(QWheelEvent *e) {
    const double z = std::clamp(zoom_ * std::pow(1.0015, e->angleDelta().y()), 0.5, 8.0);
    const QPointF c = e->position();
    pan_ = c - (c - pan_) * (z / zoom_); // keep the point under the cursor
    zoom_ = z;
    Q_EMIT viewChanged();
    update();
}

void DrawingItem::deleteSelected() {
    Session *s = Session::instance();
    const sv::render::DrawingSheet sheet = s->drawing_sheet(kind_);
    if (dimension_ >= 0 && dimension_ < int(sheet.dimensions.size())) {
        sv::json dims = sheet.dimensions;
        dims.erase(dims.begin() + dimension_);
        select_sheet(-1, -1);
        write("dimensions", dims);
    } else if (note_ >= 0 && note_ < int(sheet.notes.size())) {
        sv::json notes = sheet.notes;
        notes.erase(notes.begin() + note_);
        select_sheet(-1, -1);
        write("notes", notes);
    } else
        s->deleteSelection();
}

// Delete and Escape act on the picked dimension or note before Main.qml's window shortcuts (which would delete or
// deselect the rig selection instead).
bool DrawingItem::event(QEvent *e) {
    if (e->type() == QEvent::ShortcutOverride) {
        const int key = static_cast<QKeyEvent *>(e)->key();
        const bool sheet = dimension_ >= 0 || note_ >= 0;
        if (((key == Qt::Key_Delete || key == Qt::Key_Backspace) && sheet) || key == Qt::Key_Escape) {
            e->accept();
            return true;
        }
    }
    return QQuickPaintedItem::event(e);
}

void DrawingItem::keyPressEvent(QKeyEvent *e) {
    switch (e->key()) {
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        deleteSelected();
        break;
    case Qt::Key_Escape:
        select_sheet(-1, -1);
        Session::instance()->selectUuid(QString(), false);
        tool_ = QStringLiteral("select");
        Q_EMIT toolChanged();
        break;
    case Qt::Key_N:
        tool_ = QStringLiteral("note");
        Q_EMIT toolChanged();
        break;
    default:
        e->ignore();
    }
}
