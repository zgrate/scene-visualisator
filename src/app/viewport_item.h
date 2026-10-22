#pragma once
#include <QQuickRhiItem>
#include <qqmlintegration.h>

// The 3D view. A QQuickRhiItem so the scene renders on Qt Quick's render thread through QRhi.
class ViewportItem : public QQuickRhiItem {
    Q_OBJECT
    QML_ELEMENT

  protected:
    QQuickRhiItemRenderer *createRenderer() override;
};
