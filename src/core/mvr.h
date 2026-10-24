#pragma once
// FR-22 MVR 1.6 exchange (decision B5): a zip of GeneralSceneDescription.xml plus the GDTF and geometry files.
// Objects keep their MVR UUIDs, and every MVR child element we do not interpret is kept verbatim in
// `extras.mvr.xml`, so import -> export -> import loses nothing (AT-1).
#include "core/interchange.h"
#include "core/project_file.h"

#include <array>
#include <span>

namespace sv {

struct MvrScene {
    json layers = json::array();  // {uuid, name, visible, locked}
    json objects = json::array(); // fixture, truss, ledWall, setPiece or group, with `layer` and `extras.mvr`
    ZipEntries files;             // package entries: gdtf/<GDTFSpec>, geometry/<file>
    std::string aux_data;         // <AUXData> verbatim (symbols, classes, positions); goes to project["mvr"]
    std::vector<std::string> warnings;
};
// Type and mode text -> {library GDTF file, mode}, asked for fixtures whose GDTF is not in the MVR (grandMA3 exports
// none). An empty file keeps the text and warns.
using TypeResolver = std::function<std::pair<std::string, std::string>(const std::string &, const std::string &)>;
MvrScene mvr_import(std::span<const std::uint8_t> zip, const TypeResolver &resolve = {}); // throws InterchangeError

// files: the package entries (gdtf/, geometry/). Walls, trusses and OBJ set pieces without MVR geometry get a
// generated .glb. Pixel strips and cameras have no MVR counterpart and are left out.
std::vector<std::uint8_t> mvr_export(const json &project, const ZipEntries &files);

// An MVR matrix {u}{v}{w}{o}: u, v, w are the columns (local axes), o the origin in mm; Z up, Y upstage.
// Ours: metres, Y up, Z toward the audience, rotation in degrees applied as Ry·Rx·Rz, then scale.
struct Transform {
    std::array<double, 3> position{}, rotation{}, scale{1, 1, 1};
};
Transform transform_from_mvr(const std::array<double, 12> &m);
// The same matrix layout in our frame (metres): columns are the local axes, then the origin.
Transform transform_from_matrix(const std::array<double, 12> &m);
std::array<double, 12> mvr_from_transform(const Transform &t);

// glTF 2.0 (.glb, or .gltf with embedded buffers) to a triangle soup, one group per mesh node, node transforms
// applied. glTF is metres and Y up, which is our object frame. Throws InterchangeError.
std::vector<ObjGroup> parse_gltf(std::span<const std::uint8_t> bytes);
// A .glb holding one triangle list (x y z per vertex, metres).
std::vector<std::uint8_t> write_glb(const std::vector<float> &positions);

} // namespace sv
