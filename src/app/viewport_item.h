#pragma once
#include "render/camera.h"

#include <QElapsedTimer>
#include <QPointF>
#include <QQuickRhiItem>
#include <QTimer>
#include <QVariantMap>
#include <QVector3D>
#include <optional>
#include <qqmlintegration.h>
#include <string>

// The 3D view. A QQuickRhiItem so the scene renders on Qt Quick's render thread through QRhi.
// Left drag orbits, middle or Shift+left drag pans, the wheel zooms, a click selects (Ctrl/Cmd adds).
// Left drag on a selected object moves it across the floor plane, snapped to 0.25 m (Alt: free).
class ViewportItem : public QQuickRhiItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString view READ view NOTIFY viewChanged) // "Perspective", "Top", ...

  public:
    explicit ViewportItem(QQuickItem *parent = nullptr);

    QString view() const { return view_; }
    Q_INVOKABLE void setPreset(const QString &name); // Perspective, Top, Front, Side
    Q_INVOKABLE QVariantMap cameraState() const;     // for bookmarks in the project layout
    Q_INVOKABLE void setCameraState(const QVariantMap &state);

    sv::render::OrbitCamera camera;
    QElapsedTimer clock;

  Q_SIGNALS:
    void viewChanged();

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
    QString view_ = QStringLiteral("Perspective");
    struct Grab {
        std::string uuid;
        QVector3D start, offset; // object position, and grab point minus position on its height plane
    };
    std::optional<Grab> grab;
    std::optional<QVector3D> on_plane(QPointF p, float y) const;
};
