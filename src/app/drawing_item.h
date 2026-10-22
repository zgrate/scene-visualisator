#pragma once
#include <QQuickPaintedItem>
#include <QTransform>
#include <qqmlintegration.h>

// FR-19 sheet preview: the drawing painter on a paper-white page fitted into the item. Shift-drag adds a manual
// dimension line, stored in the drawing (one undo step).
class DrawingItem : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString kind MEMBER kind_ NOTIFY kindChanged) // plan, front, side

  public:
    explicit DrawingItem(QQuickItem *parent = nullptr);
    void paint(QPainter *p) override;

  Q_SIGNALS:
    void kindChanged();

  protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

  private:
    QString kind_ = QStringLiteral("plan");
    QTransform to_item_; // drawing metres -> item pixels, from the last paint
    QPointF drag_from_, drag_to_;
    bool dragging_ = false;
};
