#pragma once
// Document + decoded DMX -> RenderScene (plain instance lists the GPU passes draw). Runs on the render thread.
#include "core/model.h"
#include "gdtf/decoder.h"
#include "protocol/generators.h"

#include <QMatrix4x4>
#include <QVector2D>
#include <QVector4D>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace sv::render {

using FixtureLibrary = std::map<std::string, gdtf::FixtureType, std::less<>>; // by GDTF file name

struct FixtureState {
    const gdtf::FixtureType *type = nullptr; // null: type file missing, drawn as a grey box without a beam
    const gdtf::Mode *mode = nullptr;
    gdtf::Attributes attrs;
};

// Set-piece geometry: x y z nx ny nz per vertex, three vertices per triangle (sv::parse_obj layout), plus bounds.
struct Mesh {
    std::vector<float> vertices;
    QVector3D lo, hi;
};
std::shared_ptr<const Mesh> make_mesh(std::vector<float> vertices);
// By package path ("geometry/stage.obj") and per OBJ group ("geometry/stage.obj#Truss").
using MeshLibrary = std::map<std::string, std::shared_ptr<const Mesh>, std::less<>>;

// owner: index into project["objects"], -1 for venue parts (decks). Used by OBJ export.
struct BoxInstance {
    QMatrix4x4 model; // unit cube centred at the origin
    QVector4D colour;
    int owner = -1;
};
struct MeshInstance {
    std::shared_ptr<const Mesh> mesh;
    QMatrix4x4 model;
    QVector4D colour;
    int owner = -1;
};
struct BeamInstance {
    QVector3D origin, direction;
    float length, tan_half_angle;
    QVector3D colour; // already scaled by intensity
};
struct WallInstance {
    QMatrix4x4 model; // unit quad [-0.5, 0.5]^2 in local XY, front facing +Z
    QVector4D slice;  // composition rect in 0..1 (x, y, w, h), origin top-left
    QVector4D grid;   // pixels x, pixels y, modules x, modules y
    float brightness; // nits relative to 1500
    bool selected;
    int owner = -1;
    float angle = 0;  // slice rotation about its centre, degrees, clockwise on screen (Arena InputRect)
    float aspect = 1; // composition width / height, so the rotation keeps pixels square
};
struct FloorSpot {
    QVector2D centre;
    float radius;
    QVector3D colour;
};
struct PickBox {
    QMatrix4x4 model; // unit cube bounds
    std::string uuid;
};

struct RenderScene {
    float venue_width = 20, venue_depth = 15, haze = 0.35f;
    std::vector<BoxInstance> boxes;
    std::vector<MeshInstance> meshes;
    std::vector<BeamInstance> beams;
    std::vector<WallInstance> walls;
    std::vector<FloorSpot> spots;
    std::vector<PickBox> pick;
};

using FrameLookup = std::function<std::span<const std::uint8_t>(std::uint16_t universe)>; // empty span: no data

std::map<std::string, FixtureState> decode_fixtures(const json &project, const FixtureLibrary &library,
                                                    const FrameLookup &frame);
RenderScene build_scene(const json &project, const std::map<std::string, FixtureState> &fixtures,
                        const std::set<std::string> &selected, double time_s, const MeshLibrary &meshes = {});
// FR-25 OBJ export of the scene at rest (no DMX): one group per scene object, walls as named quads, beams left out.
// only: uuids to export, all objects when empty. Metres, Y-up.
std::string export_obj(const json &project, const FixtureLibrary &library, const MeshLibrary &meshes,
                       const std::set<std::string> &only = {});
// What the internal generators (FR-11) drive for each patched fixture of a known type.
std::vector<proto::GeneratorTarget> generator_targets(const json &project, const FixtureLibrary &library);

// Nearest object hit by a world-space ray (direction normalised).
std::optional<std::string> pick(const RenderScene &scene, QVector3D origin, QVector3D direction);

} // namespace sv::render
