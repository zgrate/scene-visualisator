#pragma once
#include <QColor>
#include <QFont>
#include <QQuickPaintedItem>
#include <QVariantList>
#include <array>
#include <cstdint>
#include <qqmlintegration.h>

// DMX monitor grid: 512 channels as 32 x 16 cells with the value, a level fill, the selected fixture's channels
// outlined, patch conflicts flagged, and a stripe in the colour of the source that won each channel.
class ChannelGrid : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int universe MEMBER universe_ NOTIFY universeChanged)
    Q_PROPERTY(QFont font MEMBER font_ NOTIFY styleChanged)
    Q_PROPERTY(QColor textColor MEMBER text_ NOTIFY styleChanged)
    Q_PROPERTY(QColor mutedColor MEMBER muted_ NOTIFY styleChanged)
    Q_PROPERTY(QColor lineColor MEMBER line_ NOTIFY styleChanged)
    Q_PROPERTY(QColor levelColor MEMBER fill_ NOTIFY styleChanged)
    Q_PROPERTY(QColor highlightColor MEMBER highlight_ NOTIFY styleChanged)
    Q_PROPERTY(QColor conflictColor MEMBER conflict_ NOTIFY styleChanged)
    Q_PROPERTY(QVariantList sourceColors MEMBER source_colours_ NOTIFY styleChanged) // winner stripe, by source index

  public:
    explicit ChannelGrid(QQuickItem *parent = nullptr);
    void paint(QPainter *p) override;

  Q_SIGNALS:
    void universeChanged();
    void styleChanged();

  private:
    void pull();

    int universe_ = 1;
    QFont font_;
    QColor text_, muted_, line_, fill_, highlight_, conflict_;
    QVariantList source_colours_;
    std::array<std::uint8_t, 512> values_{}, winner_{}, occupancy_{};
};
