#include "render/transform_math.h"

namespace sv::render {

std::optional<float> axis_param(QVector3D ro, QVector3D rd, QVector3D lo, QVector3D ld) {
    // Closest points of two lines: solve for t on the axis line.
    const QVector3D w = lo - ro;
    const float a = QVector3D::dotProduct(rd, rd), b = QVector3D::dotProduct(rd, ld), c = QVector3D::dotProduct(ld, ld);
    const float d = QVector3D::dotProduct(rd, w), e = QVector3D::dotProduct(ld, w);
    const float denom = a * c - b * b;
    if (std::abs(denom) < 1e-6f * a * c)
        return std::nullopt;
    return (b * d - a * e) / denom;
}

std::optional<QVector3D> plane_hit(QVector3D ro, QVector3D rd, QVector3D po, QVector3D n) {
    const float dn = QVector3D::dotProduct(rd, n);
    if (std::abs(dn) < 1e-6f)
        return std::nullopt;
    const float t = QVector3D::dotProduct(po - ro, n) / dn;
    return t > 0 ? std::optional(ro + rd * t) : std::nullopt;
}

std::optional<double> parse_typed(const QString &text) {
    bool ok = false;
    const double v = text.toDouble(&ok);
    return ok ? std::optional(v) : std::nullopt;
}

} // namespace sv::render
