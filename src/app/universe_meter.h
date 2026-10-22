#pragma once
#include <QColor>
#include <QQuickItem>
#include <array>
#include <cstdint>
#include <qqmlintegration.h>

// 512 DMX levels as bars in one geometry node; a Repeater of 512 Rectangles is far too slow at 20 Hz.
class UniverseMeter : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int universe MEMBER universe_ NOTIFY universeChanged)
    Q_PROPERTY(QColor color MEMBER color_ NOTIFY colorChanged)

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
    QColor color_{0x4c, 0x8d, 0xff};
    std::array<std::uint8_t, 512> levels_{};
};
