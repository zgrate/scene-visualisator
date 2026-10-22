#pragma once
#include <QColor>
#include <QQuickItem>
#include <array>
#include <cstdint>
#include <qqmlintegration.h>

// 512 DMX levels as bars in one geometry node; a Repeater of 512 Rectangles is far too slow at 20 Hz.
// With occupancy set it shows the patch instead: full bars for used channels, conflicts and the selection coloured.
class UniverseMeter : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int universe MEMBER universe_ NOTIFY universeChanged)
    Q_PROPERTY(QColor color MEMBER color_ NOTIFY colorChanged)
    Q_PROPERTY(bool occupancy MEMBER occupancy_ NOTIFY universeChanged)
    Q_PROPERTY(QColor conflictColor MEMBER conflict_ NOTIFY colorChanged)
    Q_PROPERTY(QColor highlightColor MEMBER highlight_ NOTIFY colorChanged)

  public:
    explicit UniverseMeter(QQuickItem *parent = nullptr);

  Q_SIGNALS:
    void universeChanged();
    void colorChanged();

  protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &now, const QRectF &before) override {
        QQuickItem::geometryChange(now, before);
        update();
    }

  private:
    void pull();

    int universe_ = 1;
    bool occupancy_ = false;
    QColor color_{0x4c, 0x8d, 0xff}, conflict_{0xff, 0x6b, 0x6b}, highlight_{0xff, 0xd1, 0x66};
    std::array<std::uint8_t, 512> levels_{}; // DMX levels, or Session::occupancy() states in occupancy mode
};
