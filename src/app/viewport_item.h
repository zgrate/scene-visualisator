#pragma once
#include "render/camera.h"

#include <QElapsedTimer>
#include <QPointF>
#include <QQuickRhiItem>
#include <QTimer>
#include <qqmlintegration.h>

// The 3D view. A QQuickRhiItem so the scene renders on Qt Quick's render thread through QRhi.
// Left drag orbits, middle or Shift+left drag pans, the wheel zooms, a click selects (Ctrl/Cmd adds).
class ViewportItem : public QQuickRhiItem {
    Q_OBJECT
    QML_ELEMENT

  public:
    explicit ViewportItem(QQuickItem *parent = nullptr);

    sv::render::OrbitCamera camera;
    QElapsedTimer clock;

  protected:
    QQuickRhiItemRenderer *createRenderer() override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;

  private:
    QTimer tick;
    QPointF last, press;
    bool dragged = false;
};
