#include "render/camera.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace sv::render {

namespace {
constexpr float deg = std::numbers::pi_v<float> / 180;
}

QVector3D OrbitCamera::eye() const {
    const float p = pitch * deg, y = yaw * deg;
    return target + distance * QVector3D(std::cos(p) * std::sin(y), std::sin(p), std::cos(p) * std::cos(y));
}

QMatrix4x4 OrbitCamera::view() const {
    QMatrix4x4 m;
    // Straight down (or up) needs another up vector; keep the audience at the bottom of the screen.
    const QVector3D up = pitch > 89    ? QVector3D(-std::sin(yaw * deg), 0, -std::cos(yaw * deg))
                         : pitch < -89 ? QVector3D(std::sin(yaw * deg), 0, std::cos(yaw * deg))
                                       : QVector3D(0, 1, 0);
    m.lookAt(eye(), target, up);
    return m;
}

QMatrix4x4 OrbitCamera::projection(float aspect) const {
    QMatrix4x4 m;
    m.perspective(fov, aspect, 0.05f, 500.0f);
    return m;
}

void OrbitCamera::orbit(float dyaw, float dpitch) {
    yaw = std::fmod(yaw + dyaw, 360.0f);
    pitch = std::clamp(pitch + dpitch, -89.9f, 89.9f);
}

void OrbitCamera::look(float dyaw, float dpitch) {
    const QVector3D e = eye();
    orbit(dyaw, -dpitch); // the eye stays put, so the target swings the other way round it
    target += e - eye();
}

void OrbitCamera::fly(QVector3D local) {
    const QVector3D forward = (target - eye()).normalized();
    const QVector3D right = QVector3D::crossProduct(forward, {0, 1, 0}).normalized();
    target += local.x() * right + local.y() * QVector3D(0, 1, 0) + local.z() * forward;
}

void OrbitCamera::flip() {
    yaw = std::fmod(yaw + 180, 360.0f);
    pitch = -pitch;
}

void OrbitCamera::pan(float dx, float dy) {
    const QVector3D forward = (target - eye()).normalized();
    const QVector3D right = QVector3D::crossProduct(forward, {0, 1, 0}).normalized();
    const QVector3D up = QVector3D::crossProduct(right, forward);
    const float scale = 2 * distance * std::tan(fov * deg / 2); // world height of the view at the target
    target += (-dx * right + dy * up) * scale;
}

void OrbitCamera::zoom(float steps) { distance = std::clamp(distance * std::pow(0.9f, steps), 0.5f, 300.0f); }

void OrbitCamera::set_preset(Preset p) {
    switch (p) {
    case Preset::perspective:
        *this = OrbitCamera{};
        break;
    case Preset::top:
        yaw = 0, pitch = 89.9f;
        break;
    case Preset::front:
        yaw = 0, pitch = 3;
        break;
    case Preset::side:
        yaw = 90, pitch = 3;
        break;
    case Preset::back:
        yaw = 180, pitch = 3;
        break;
    case Preset::left:
        yaw = -90, pitch = 3;
        break;
    case Preset::bottom:
        yaw = 0, pitch = -89.9f;
        break;
    }
}

void OrbitCamera::frame(QVector3D centre, float radius) {
    target = centre;
    distance = std::clamp(std::max(radius, 0.25f) / std::sin(fov * deg / 2) * 1.1f, 0.5f, 300.0f);
}

std::pair<QVector3D, QVector3D> OrbitCamera::ray(float x, float y, float aspect) const {
    const QMatrix4x4 inv = (projection(aspect) * view()).inverted();
    const float nx = 2 * x - 1, ny = 1 - 2 * y;
    const QVector3D near = inv.map(QVector3D(nx, ny, -1)), far = inv.map(QVector3D(nx, ny, 1));
    return {near, (far - near).normalized()};
}

} // namespace sv::render
