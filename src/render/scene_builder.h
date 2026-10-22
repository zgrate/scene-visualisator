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
#include <optional>
#include <set>

namespace sv::render {

using FixtureLibrary = std::map<std::string, gdtf::FixtureType, std::less<>>; // by GDTF file name

struct FixtureState {
    const gdtf::FixtureType *type = nullptr; // null: type file missing, drawn as a grey box without a beam
    const gdtf::Mode *mode = nullptr;
    gdtf::Attributes attrs;
};

struct BoxInstance {
    QMatrix4x4 model; // unit cube centred at the origin
    QVector4D colour;
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
    std::vector<BeamInstance> beams;
    std::vector<WallInstance> walls;
    std::vector<FloorSpot> spots;
    std::vector<PickBox> pick;
};

using FrameLookup = std::function<std::span<const std::uint8_t>(std::uint16_t universe)>; // empty span: no data

std::map<std::string, FixtureState> decode_fixtures(const json &project, const FixtureLibrary &library,
                                                    const FrameLookup &frame);
RenderScene build_scene(const json &project, const std::map<std::string, FixtureState> &fixtures,
                        const std::set<std::string> &selected, double time_s);
// What the internal generators (FR-11) drive for each patched fixture of a known type.
std::vector<proto::GeneratorTarget> generator_targets(const json &project, const FixtureLibrary &library);

// Nearest object hit by a world-space ray (direction normalised).
std::optional<std::string> pick(const RenderScene &scene, QVector3D origin, QVector3D direction);

} // namespace sv::render
