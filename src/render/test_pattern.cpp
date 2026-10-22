#include "render/test_pattern.h"

#include <QPainter>
#include <algorithm>

namespace sv::render {

QImage make_composition(const json &project, const QImage &still) {
    const json &comp = project.value("composition", json::object());
    const int w = std::clamp(comp.value("width", 1920), 16, 4096), h = std::clamp(comp.value("height", 1080), 16, 4096);
    const json &src = comp.value("source", json::object());
    const std::string kind = src.is_object() ? src.value("kind", std::string("testPattern")) : "testPattern";
    QImage img(w, h, QImage::Format_RGBA8888);
    img.fill(Qt::black);
    QPainter p(&img);
    QFont font;
    font.setPixelSize(std::max(12, h / 24));
    font.setBold(true);
    p.setFont(font);
    if (kind == "testPattern") {
        static const QColor bars[] = {{192, 192, 192}, {192, 192, 0}, {0, 192, 192}, {0, 192, 0},
                                      {192, 0, 192},   {192, 0, 0},   {0, 0, 192}};
        for (int i = 0; i < 7; ++i)
            p.fillRect(QRectF(i * w / 7.0, 0, w / 7.0 + 1, h * 0.7), bars[i]);
        for (int i = 0; i < 16; ++i) // grey ramp
            p.fillRect(QRectF(i * w / 16.0, h * 0.7, w / 16.0 + 1, h * 0.3), QColor::fromRgb(i * 17, i * 17, i * 17));
    } else if (kind == "grid") { // mapping grid: 1/16 of the height per cell, centre cross, pixel coordinates
        const int cell = std::max(8, h / 16);
        p.setPen(QPen(QColor(70, 70, 70), 1));
        for (int x = 0; x <= w; x += cell)
            p.drawLine(x, 0, x, h);
        for (int y = 0; y <= h; y += cell)
            p.drawLine(0, y, w, y);
        p.setPen(QPen(QColor(0, 160, 255), std::max(2, h / 360)));
        p.drawLine(w / 2, 0, w / 2, h);
        p.drawLine(0, h / 2, w, h / 2);
        p.setPen(QColor(150, 150, 150));
        QFont small = font;
        small.setPixelSize(std::max(10, cell / 4));
        small.setBold(false);
        p.setFont(small);
        for (int x = 0; x < w; x += cell * 2)
            for (int y = 0; y < h; y += cell * 2)
                p.drawText(x + 4, y + small.pixelSize() + 2, QStringLiteral("%1,%2").arg(x).arg(y));
        p.setFont(font);
    } else {
        if (!still.isNull()) { // a still, or the first frame shown for a live source
            const std::string fit = src.value("fit", std::string("fit"));
            QSizeF size = still.size();
            if (fit == "stretch")
                size = QSizeF(w, h);
            else
                size.scale(w, h, fit == "fill" ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio);
            p.drawImage(QRectF((w - size.width()) / 2, (h - size.height()) / 2, size.width(), size.height()), still);
        } else {
            p.setPen(QColor(255, 209, 102));
            p.drawText(QRect(0, 0, w, h), Qt::AlignCenter, kind == "still" ? "Missing image" : "No signal");
        }
        return img; // live media and stills carry no slice overlay
    }
    for (const json &o : project.value("objects", json::array())) {
        const std::string type = o.value("type", std::string());
        if (type != "ledWall" && !(type == "pixelStrip" && o.contains("slice")))
            continue;
        const json &s = o.value("slice", json::object());
        const double sw = s.value("w", double(w)), sh = s.value("h", double(h));
        p.save();
        p.translate(s.value("x", 0.0) + sw / 2, s.value("y", 0.0) + sh / 2);
        p.rotate(s.value("angle", 0.0));
        const QRect r = QRectF(-sw / 2, -sh / 2, sw, sh).toRect();
        p.setPen(QPen(Qt::white, std::max(2, h / 270)));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r.adjusted(1, 1, -2, -2));
        if (type == "pixelStrip") { // thin and numerous: outline only
            p.restore();
            continue;
        }
        p.drawLine(r.topLeft(), r.bottomRight());
        p.drawLine(r.topRight(), r.bottomLeft());
        const QString label = QString::fromStdString(o.value("name", std::string()));
        const QRect text = p.fontMetrics().boundingRect(r, Qt::AlignCenter, label).adjusted(-8, -4, 8, 4);
        p.fillRect(text, Qt::black);
        p.drawText(r, Qt::AlignCenter, label);
        p.restore();
    }
    return img;
}

} // namespace sv::render
