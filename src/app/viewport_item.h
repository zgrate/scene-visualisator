#pragma once
#include "core/model.h"
#include "render/camera.h"

#include <QElapsedTimer>
#include <QMatrix4x4>
#include <QPointF>
#include <QQuickRhiItem>
#include <QTimer>
#include <QVariantMap>
#include <QVector3D>
#include <atomic>
#include <optional>
#include <qqmlintegration.h>
#include <set>
#include <string>

// The 3D view. A QQuickRhiItem so the scene renders on Qt Quick's render thread through QRhi.
// Mouse follows Session.controls: "blender" (middle drag orbits, Shift pans, Ctrl zooms) or "maya" (Alt +
// left/middle/right drag orbits/pans/zooms). Both: wheel zooms, click selects (Shift or Ctrl adds), left drag on a
// selected object or a gizmo handle moves, turns or scales it (snapped; Alt: free), right drag flies with WASD QE.
// Keys come from the table in view_keys.h. In the "floor" tool a click emits floorClicked instead of selecting.
class ViewportItem : public QQuickRhiItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString view READ view NOTIFY viewChanged)                                   // "Perspective", "Top", ...
    Q_PROPERTY(QString gizmoMode READ gizmoMode WRITE setGizmoMode NOTIFY gizmoModeChanged) // all|move|rotate|scale
    Q_PROPERTY(QString tool READ tool WRITE setTool NOTIFY toolChanged)                     // select|floor
    Q_PROPERTY(bool keys MEMBER keys_) // off while another screen covers it
    Q_PROPERTY(QString transformHint READ transformHint NOTIFY transformHintChanged) // "Move X: 1.25 m"
    Q_PROPERTY(QString flying READ flying NOTIFY flyingChanged)                      // "", "hold" or "walk"

  public:
    explicit ViewportItem(QQuickItem *parent = nullptr);

    QString view() const { return view_; }
    QString gizmoMode() const { return gizmo_mode_; }
    void setGizmoMode(const QString &mode);
    QString tool() const { return tool_; }
    void setTool(const QString &tool);
    QString transformHint() const { return hint_; }
    QString flying() const { return fly_ == Fly::hold ? "hold" : fly_ == Fly::walk ? "walk" : ""; }
    std::string gizmo_entry() const; // the selection pseudo-entry the renderer passes to build_scene, or empty
    static bool hdr();               // preference renderQuality: "high" (default: HDR, bloom, haze) or "fast"

    Q_INVOKABLE void setPreset(const QString &name); // Perspective, Top, Front, Side, Back, Other side, Bottom
    Q_INVOKABLE QVariantMap cameraState() const;     // for bookmarks in the project layout
    Q_INVOKABLE void setCameraState(const QVariantMap &state);
    Q_INVOKABLE void frameSelection(bool all = false);    // the selection, or the whole rig (all, or nothing selected)
    Q_INVOKABLE QVariantList keyHelp() const;             // {group, keys, label} rows for the keyboard card
    Q_INVOKABLE bool beginTransform(const QString &kind); // modal "move", "rotate" or "scale" of the selection
    Q_INVOKABLE QVariantList floorPoint(qreal x, qreal y) const; // [x, 0, z] snapped to 0.25 m, [] off the floor

    sv::render::OrbitCamera camera;
    QElapsedTimer clock;
    std::atomic_bool animating{false}; // set by the renderer while a lit fixture strobes, so the tick keeps drawing

  Q_SIGNALS:
    void viewChanged();
    void gizmoModeChanged();
    void toolChanged();
    void transformHintChanged();
    void floorClicked(QVector3D point);
    void flyingChanged();

  protected:
    QQuickRhiItemRenderer *createRenderer() override;
    bool event(QEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void hoverMoveEvent(QHoverEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;

  private:
    enum class Nav { none, orbit, pan, zoom };
    enum class Fly { none, hold, walk }; // hold: right button down; walk: Shift+` until Enter or Esc
    // One move, turn or scale in progress: from a gizmo handle, a body drag or a modal G/R/S.
    struct Edit {
        std::string uuid, kind, handle; // kind move|rotate|scale; handle tx|ty|tz|ry|s|plane|modal
        sv::json start;                 // the object's position, rotation and scale before the edit
        QMatrix4x4 parent;              // parent world transform (identity at the top level)
        QMatrix4x4 frame;               // gizmo frame: parent axes, unscaled, at the handle origin
        QVector3D origin, grab;         // object origin in world; first hit on the drag plane
        QPointF mouse0;                 // cursor at the start (modal)
        double t0 = 0;                  // axis parameter, angle or radius at the start
        int axis = -1;                  // modal axis lock 0..2
        QString typed;                  // modal typed value
    };
    QTimer tick;
    std::vector<quint64> drawn_; // what the last redraw saw (DMX, media, document); see the tick
    Fly fly_ = Fly::none;
    std::set<int> held_;                // keys down while flying
    float fly_speed_ = 4;               // m/s, the wheel scales it while flying
    sv::render::OrbitCamera fly_start_; // restored when walk is cancelled
    QElapsedTimer fly_clock_;
    QPointF last, press;
    bool dragged = false, keys_ = true, modal_ = false;
    Nav nav_ = Nav::none;
    QString view_ = QStringLiteral("Perspective"), gizmo_mode_ = QStringLiteral("all"),
            tool_ = QStringLiteral("select"), hint_;
    std::optional<std::string> press_hit;
    std::optional<Edit> edit_;

    std::pair<QVector3D, QVector3D> ray_at(QPointF p) const;
    std::optional<QVector3D> on_plane(QPointF p, float y) const;
    QPointF to_screen(QVector3D world) const;
    bool start_edit(const std::string &uuid, const std::string &handle, const std::optional<QMatrix4x4> &frame);
    void drag_handle(QPointF p, bool free);
    void drag_modal(QPointF p, bool free);
    void finish(bool commit);
    void set_hint(const QString &hint);
    void set_fly(Fly fly);
    void step_fly();
};
