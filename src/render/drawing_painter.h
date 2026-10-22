#pragma once
// FR-19 plan and elevation drawings: an orthographic view of the scene at rest, painted with QPainter in page
// millimetres, so the same code draws on screen (DrawingItem), to PDF and to SVG.
#include "render/scene_builder.h"

#include <QPainter>
#include <QSizeF>
#include <QTransform>

namespace sv::render {

// One entry of project["drawings"]: {uuid, kind, sheet, scale, title, hiddenLayers, dimensions}.
struct DrawingSheet {
    std::string kind = "plan"; // plan (from above, audience at the bottom), front (from the audience), side (from
                               // stage left, audience on the left)
    std::string paper = "A3";  // A3 or A4, landscape
    int scale = 0;             // 1:N, 0 fits the sheet
    std::string title;         // empty: "Plan", "Front elevation" or "Side elevation"
    std::set<std::string> hidden_layers;
    json dimensions = json::array(); // manual dimension lines [{a: [u, v], b: [u, v]}], drawing metres
    std::string project_name;        // title block, filled by the caller (the file name)
    bool text = true;                // false leaves out all lettering (golden images do not depend on fonts)
};
DrawingSheet sheet_from_json(const json &drawing);
QSizeF sheet_size_mm(const DrawingSheet &sheet); // landscape
// World point -> drawing coordinates in metres: u right, v down the page.
QPointF drawing_point(const std::string &kind, QVector3D p);

// Paints the whole sheet into page millimetres (0,0)-(sheet_size_mm). Returns drawing metres -> page millimetres,
// which callers invert to place manual dimensions.
QTransform paint_drawing(QPainter &p, const json &project, const FixtureLibrary &library, const MeshLibrary &meshes,
                         const DrawingSheet &sheet);

} // namespace sv::render
