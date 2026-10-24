#pragma once
// FR-19 plan and elevation drawings: an orthographic view of the scene at rest, painted with QPainter in page
// millimetres, so the same code draws on screen (DrawingItem), to PDF and to SVG.
#include "render/scene_builder.h"

#include <QPainter>
#include <QSizeF>
#include <QTransform>

namespace sv::render {

// One entry of project["drawings"]: {uuid, kind, sheet, scale, title, hiddenLayers, dimensions, notes, autoDimensions,
// labels, legend, schedule}.
struct DrawingSheet {
    std::string kind = "plan"; // plan (from above, audience at the bottom), front (from the audience), side (from
                               // stage left, audience on the left)
    std::string paper = "A3";  // A3 or A4, landscape
    int scale = 0;             // 1:N, 0 fits the sheet
    std::string title;         // empty: "Plan", "Front elevation" or "Side elevation"
    std::set<std::string> hidden_layers;
    json dimensions = json::array(); // manual dimension lines [{a: [u, v], b: [u, v], offset?: mm}], drawing metres
    json notes = json::array();      // [{at: [u, v], text, to?: [u, v]}], drawing metres; `to` is the leader's tip
    bool auto_dimensions = true;     // overall stage, fixture rows and truss trims
    bool labels = true;              // fixture ID and patch, truss and wall names
    bool legend = true;              // fixture symbols per type with counts, left of the title block
    bool schedule = false;           // fixture table (ID, name, type, mode, patch) in a column on the right
    std::set<std::string> selected;  // uuids drawn in the highlight colour (on screen only)
    std::string project_name;        // title block, filled by the caller (the file name)
    bool text = true;                // false leaves out all lettering (golden images do not depend on fonts)
};
DrawingSheet sheet_from_json(const json &drawing);
QSizeF sheet_size_mm(const DrawingSheet &sheet); // landscape
// World point -> drawing coordinates in metres: u right, v down the page.
QPointF drawing_point(const std::string &kind, QVector3D p);
// Inverse of drawing_point for a displacement: a drag on the sheet -> world delta (the hidden axis stays 0).
QVector3D drawing_delta(const std::string &kind, QPointF d);

// What paint_drawing drew where, for picking on screen: one entry per fixture symbol, per other object's bounds and
// per note ("note:<index>", only when lettering is on).
struct DrawingHit {
    std::string uuid;
    QRectF page; // page millimetres
};

// Paints the whole sheet into page millimetres (0,0)-(sheet_size_mm). Returns drawing metres -> page millimetres,
// which callers invert to place manual dimensions. `hits`, when given, receives the pickable objects.
QTransform paint_drawing(QPainter &p, const json &project, const FixtureLibrary &library, const MeshLibrary &meshes,
                         const DrawingSheet &sheet, std::vector<DrawingHit> *hits = nullptr);

} // namespace sv::render
