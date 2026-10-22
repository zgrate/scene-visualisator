#pragma once
// File interchange (docs "Interchange formats"): patch CSV, grandMA3 patch XML, Resolume Advanced Output XML,
// OBJ geometry. Pure functions over the project JSON; edits come back as JSON patches for Document::apply.
#include "core/model.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sv {

struct InterchangeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string patch_csv(const json &project);     // FR-8, FR-23
std::string ma3_patch_xml(const json &project); // FR-23
std::string resolume_xml(const json &project);  // FR-24
// FR-24 import: walls matched by name get the slice and the size implied by the output rect; unknown screens become
// new walls. Also sets the composition size. Throws InterchangeError.
json resolume_import_patch(const json &project, std::string_view xml);

// Triangle soup: x y z nx ny nz per vertex, three vertices per triangle, flat normals.
struct ObjGroup {
    std::string name; // "o" or "g" name, "default" before the first one
    std::vector<float> vertices;
};
// Polygons are fanned into triangles; vt/vn and materials are ignored.
// shortcut: no MTL materials (set pieces render grey), switch to tinyobjloader when materials are needed.
std::vector<ObjGroup> parse_obj(std::string_view text); // throws InterchangeError on bad indices or no faces

// Scale for an OBJ unit name: "m", "cm", "mm", "in", "ft".
double unit_scale(std::string_view unit);

} // namespace sv
