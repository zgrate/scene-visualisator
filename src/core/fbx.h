#pragma once
// FR-25 FBX (B5). Import goes through ufbx; export is our own binary FBX 7.4 writer, Y up, metres
// (UnitScaleFactor 100), which Blender, Unreal and Unity read without a conversion dialog.
#include "core/interchange.h"
#include "core/mvr.h"

#include <span>

namespace sv {

struct FbxNode {
    std::string name;
    int parent = -1;     // index into nodes, -1 under the scene root; parents come first
    Transform transform; // relative to the parent, our frame
    int group = -1;      // index into FbxScene::groups when the node has a mesh
};
struct FbxScene {
    std::vector<FbxNode> nodes;   // only nodes with a mesh somewhere below them
    std::vector<ObjGroup> groups; // node-space triangles with flat normals, names unique
};
FbxScene load_fbx(std::span<const std::uint8_t> bytes); // throws InterchangeError

struct FbxMesh {
    std::string name, parent;    // parent: one of the group nulls by name, empty for the root
    std::vector<float> vertices; // world-space triangles, x y z per vertex
    std::vector<float> uvs;      // empty, or u v per vertex
    std::array<float, 4> colour{0.7f, 0.7f, 0.7f, 1};
    bool emissive = false;
};
struct FbxCamera {
    std::string name;
    std::array<double, 3> position{}, target{};
    double fov = 50; // vertical, degrees
};
// Group nulls are made for every distinct FbxMesh::parent, plus "Cameras" when there are cameras.
std::vector<std::uint8_t> write_fbx(const std::vector<FbxMesh> &meshes, const std::vector<FbxCamera> &cameras);

} // namespace sv
