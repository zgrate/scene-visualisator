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
    QVector4D colour; // material base colour (OBJ MTL, FBX); alpha 0: the default set-piece grey
};
std::shared_ptr<const Mesh> make_mesh(std::vector<float> vertices, QVector4D colour = {});
// By package path ("geometry/stage.obj") and per OBJ group ("geometry/stage.obj#Truss").
using MeshLibrary = std::map<std::string, std::shared_ptr<const Mesh>, std::less<>>;

// owner: index into project["objects"]. Used by OBJ/FBX export and drawings.
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
    QVector3D colour;         // already scaled by intensity
    int owner = -1;           // index into project["objects"]
    const char *hit = "none"; // what ends it: floor, ceiling (venue height), deck, shape, ledWall, setPiece or none
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
    // Frame of the transform gizmo (parent axes, unscaled, origin on the object's top) when one is drawn.
    std::optional<QMatrix4x4> gizmo;
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
// FR-25 FBX export of the same scene: one mesh per object under group nulls (Trusses, Walls, Fixtures, Set pieces,
// Venue), walls' screens as emissive quads UV-mapped to their slice, and camera objects plus viewport bookmarks (whole
// scene only) as FBX cameras.
std::vector<std::uint8_t> export_fbx(const json &project, const FixtureLibrary &library, const MeshLibrary &meshes,
                                     const std::set<std::string> &only = {});
// What the internal generators (FR-11) drive for each patched fixture of a known type.
std::vector<proto::GeneratorTarget> generator_targets(const json &project, const FixtureLibrary &library);

// An object's local-to-world matrix with its parents applied (T·Ry·Rx·Rz·S per level).
QMatrix4x4 world_transform(const json &project, const json &object);

// Nearest object hit by a world-space ray (direction normalised); a "gizmo:<handle>" box beats any object.
std::optional<std::string> pick(const RenderScene &scene, QVector3D origin, QVector3D direction);

} // namespace sv::render
