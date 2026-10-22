#pragma once
// File interchange (docs "Interchange formats"): patch CSV, grandMA3 patch XML, Resolume Advanced Output XML,
// OBJ geometry. Pure functions over the project JSON; edits come back as JSON patches for Document::apply.
#include "core/model.h"

#include <array>
#include <functional>
#include <map>
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
// Resolume Advanced Output (FR-24) lives in core/resolume.h.

// Triangle soup: x y z nx ny nz per vertex, three vertices per triangle, flat normals.
struct ObjGroup {
    std::string name; // "o" or "g" name, "default" before the first one
    std::vector<float> vertices;
    std::array<float, 4> colour{}; // material base colour, alpha 0 when the group has none
};
// MTL (B5): material name -> Kd diffuse colour with alpha 1.
using ObjMaterials = std::map<std::string, std::array<float, 4>, std::less<>>;
ObjMaterials parse_mtl(std::string_view text);
std::vector<std::string> obj_mtllibs(std::string_view text); // "mtllib" file names, in order
// Polygons are fanned into triangles; vt/vn are ignored.
// shortcut: one colour per group (its first usemtl) and no textures; split groups by material when mixed groups
// or textured set pieces matter.
std::vector<ObjGroup> parse_obj(std::string_view text, const ObjMaterials &materials = {}); // throws InterchangeError

// CSV patch import (FR-23): our own patch CSV, Capture and Vectorworks exports. Separator is ',', ';' or tab,
// whichever the header line has most of; quotes as RFC 4180.
std::vector<std::vector<std::string>> parse_csv(std::string_view text);
// Our fields: fixtureId, name, type, mode, universe, address, patch ("u.a"), x, y, z, rotX, rotY, rotZ.
inline constexpr std::array<const char *, 13> csv_fields{
    "fixtureId", "name", "type", "mode", "universe", "address", "patch", "x", "y", "z", "rotX", "rotY", "rotZ"};
using CsvMapping = std::map<std::string, int>; // field -> column, -1 or absent when unmapped
// shortcut: header synonyms stand in for Capture/Vectorworks presets, add real presets when users send samples.
CsvMapping guess_csv_mapping(const std::vector<std::string> &header);
struct CsvImportOptions {
    double scale = 1;  // unit_scale of the file's unit
    bool z_up = false; // file is Z-up (Vectorworks): ours = (x, z, -y)
    // Type and mode text -> {gdtf file, mode}; empty gdtf keeps the text, so the fixture shows as unknown.
    std::function<std::pair<std::string, std::string>(const std::string &, const std::string &)> resolve;
};
// Rows after the header become one patch: a row whose fixtureId matches a fixture updates the mapped fields, the
// rest add fixtures. Rows without a name, type or fixture ID are skipped.
json csv_import(const json &project, const std::vector<std::vector<std::string>> &rows, const CsvMapping &mapping,
                const CsvImportOptions &options);

// Scale for an OBJ unit name: "m", "cm", "mm", "in", "ft".
double unit_scale(std::string_view unit);

} // namespace sv
