#pragma once
// FR-5 viewport transform math, kept free of windows so it is unit-tested: gizmo handles and modal G/R/S.
#include <QString>
#include <QVector3D>
#include <cmath>
#include <optional>

namespace sv::render {

// Parameter t of the point on the line lo + t·ld closest to the ray ro + s·rd; nullopt when they are parallel.
std::optional<float> axis_param(QVector3D ro, QVector3D rd, QVector3D lo, QVector3D ld);
// Where the ray meets the plane through po with normal n, in front of the ray only.
std::optional<QVector3D> plane_hit(QVector3D ro, QVector3D rd, QVector3D po, QVector3D n);
// A number typed during a modal transform ("-1.5", ".25", "2."); nullopt while empty or only "-" / ".".
std::optional<double> parse_typed(const QString &text);
inline double snap(double v, double step) { return step > 0 ? std::round(v / step) * step : v; }

} // namespace sv::render
