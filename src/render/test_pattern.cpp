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

QImage compose_feeds(const json &project, QImage composition, const std::map<std::string, QImage> &feeds,
                     const std::map<std::string, double> &opacity) {
    if (feeds.empty() || composition.isNull() || !project.contains("objects"))
        return composition;
    std::map<std::string, QSizeF> sizes; // output uuid -> the size Resolume renders it at
    if (project.contains("outputs") && project.at("outputs").is_array())
        for (const json &out : project.at("outputs"))
            if (out.is_object() && out.contains("uuid"))
                sizes[out.value("uuid", std::string())] = QSizeF(out.value("width", 0), out.value("height", 0));
    const auto rect = [](const json &r) {
        return QRectF(r.value("x", 0.0), r.value("y", 0.0), r.value("w", 0.0), r.value("h", 0.0));
    };
    QPainter p(&composition);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    // shortcut: warps are ignored and a rotated outRect is skewed when output and frame aspects differ; handle them
    // when a show needs it.
    for (const json &o : project.at("objects")) {
        if (!o.is_object() || !o.contains("slice") || !o.contains("output") || !o.at("output").is_string())
            continue;
        const auto it = feeds.find(o.at("output").get<std::string>());
        const auto alpha = opacity.find(o.at("output").get<std::string>());
        if (it == feeds.end() || it->second.isNull() || (alpha != opacity.end() && alpha->second <= 0))
            continue;
        p.setOpacity(alpha == opacity.end() ? 1.0 : std::min(alpha->second, 1.0));
        const json &s = o.at("slice");
        const json &r = o.contains("outRect") && o.at("outRect").is_object() ? o.at("outRect") : s;
        const QRectF slice = rect(s), out = rect(r);
        double sx = 1, sy = 1; // output px -> frame px
        if (const QSizeF size = sizes[it->first]; size.width() > 0 && size.height() > 0)
            sx = it->second.width() / size.width(), sy = it->second.height() / size.height();
        const double slice_angle = s.value("angle", 0.0), out_angle = r.value("angle", 0.0);
        if (slice_angle == 0 && out_angle == 0) {
            p.drawImage(slice, it->second, QRectF(out.x() * sx, out.y() * sy, out.width() * sx, out.height() * sy));
            continue;
        }
        if (out.width() <= 0 || out.height() <= 0)
            continue;
        // Feed px -> output px -> upright outRect -> slice size -> rotated slice, both rotations about the centres.
        p.save();
        p.translate(slice.center());
        p.rotate(slice_angle);
        p.setClipRect(QRectF(-slice.width() / 2, -slice.height() / 2, slice.width(), slice.height()));
        p.scale(slice.width() / out.width(), slice.height() / out.height());
        p.rotate(-out_angle);
        p.translate(-out.center());
        p.scale(1 / sx, 1 / sy);
        QTransform turn;
        turn.translate(out.center().x(), out.center().y()).rotate(out_angle).translate(-out.center().x(),
                                                                                       -out.center().y());
        const QRectF b = turn.mapRect(out); // the rotated outRect's bounds, output px
        const QRectF src = QRectF(b.x() * sx, b.y() * sy, b.width() * sx, b.height() * sy) & it->second.rect();
        p.drawImage(src, it->second, src);
        p.restore();
    }
    return composition;
}

} // namespace sv::render
