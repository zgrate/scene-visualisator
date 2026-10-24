#pragma once
// FR-19 camera: perspective orbit/pan/zoom around a target, plus Top/Front/Side presets. Y-up, audience at +Z.
#include <QMatrix4x4>
#include <QVector3D>
#include <utility>

namespace sv::render {

struct OrbitCamera {
    enum class Preset { perspective, top, front, side, back, left, bottom };

    QVector3D target{0, 2.5f, 0};
    float yaw = 30, pitch = 18, distance = 19, fov = 50; // degrees, metres

    QVector3D eye() const;
    QMatrix4x4 view() const;
    QMatrix4x4 projection(float aspect) const; // OpenGL clip space; the renderer applies clipSpaceCorrMatrix
    void orbit(float dyaw, float dpitch);
    void pan(float dx, float dy); // in viewport fractions
    void zoom(float steps);       // positive = closer
    void set_preset(Preset p);
    void look(float dyaw, float dpitch); // turns about the eye (fly and walk mouse look)
    void fly(QVector3D local);           // metres: x right, y world up, z along the view; moves eye and target
    void flip();                         // the opposite side of the target (numpad 9)
    void frame(QVector3D centre, float radius); // looks at a sphere so it fills the view
    // World-space ray through a viewport point (0..1, origin top-left).
    std::pair<QVector3D, QVector3D> ray(float x, float y, float aspect) const;
};

} // namespace sv::render
