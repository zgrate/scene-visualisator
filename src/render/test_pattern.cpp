#include "render/test_pattern.h"

#include <QPainter>
#include <algorithm>

namespace sv::render {

QImage make_composition(const json &project) {
    const json &comp = project.value("composition", json::object());
    const int w = std::clamp(comp.value("width", 1920), 16, 4096), h = std::clamp(comp.value("height", 1080), 16, 4096);
    QImage img(w, h, QImage::Format_RGBA8888);
    img.fill(Qt::black);
    QPainter p(&img);
    static const QColor bars[] = {{192, 192, 192}, {192, 192, 0}, {0, 192, 192}, {0, 192, 0},
                                  {192, 0, 192},   {192, 0, 0},   {0, 0, 192}};
    for (int i = 0; i < 7; ++i)
        p.fillRect(QRectF(i * w / 7.0, 0, w / 7.0 + 1, h * 0.7), bars[i]);
    for (int i = 0; i < 16; ++i) // grey ramp
        p.fillRect(QRectF(i * w / 16.0, h * 0.7, w / 16.0 + 1, h * 0.3), QColor::fromRgb(i * 17, i * 17, i * 17));
    QFont font;
    font.setPixelSize(std::max(12, h / 24));
    font.setBold(true);
    p.setFont(font);
    for (const json &o : project.value("objects", json::array())) {
        if (o.value("type", std::string()) != "ledWall")
            continue;
        const json &s = o.value("slice", json::object());
        const QRect r(s.value("x", 0), s.value("y", 0), s.value("w", w), s.value("h", h));
        p.setPen(QPen(Qt::white, std::max(2, h / 270)));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r.adjusted(1, 1, -2, -2));
        p.drawLine(r.topLeft(), r.bottomRight());
        p.drawLine(r.topRight(), r.bottomLeft());
        const QString label = QString::fromStdString(o.value("name", std::string()));
        const QRect text = p.fontMetrics().boundingRect(r, Qt::AlignCenter, label).adjusted(-8, -4, 8, 4);
        p.fillRect(text, Qt::black);
        p.drawText(r, Qt::AlignCenter, label);
    }
    return img;
}

} // namespace sv::render
