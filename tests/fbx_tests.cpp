#include "core/fbx.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

namespace {
// Unit cube triangles shifted by (x, y, z).
std::vector<float> cube(float x, float y, float z) {
    std::vector<float> v;
    const float c[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    const int f[12][3] = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                          {3, 7, 6}, {3, 6, 2}, {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5}};
    for (const auto &t : f)
        for (int k : t)
            v.insert(v.end(), {c[k][0] + x, c[k][1] + y, c[k][2] + z});
    return v;
}
} // namespace

TEST_CASE("FBX export reads back through ufbx with names, groups, colours and metres (FR-25, AT-4)") {
    std::vector<sv::FbxMesh> meshes(2);
    meshes[0].name = "Truss 1";
    meshes[0].parent = "Trusses";
    meshes[0].vertices = cube(2, 5, -1);
    meshes[0].colour = {0.25f, 0.5f, 0.75f, 1};
    meshes[1].name = "Wall_screen";
    meshes[1].parent = "Walls";
    meshes[1].vertices = {0, 0, 0, 4, 0, 0, 4, 2, 0};
    meshes[1].uvs = {0, 0, 1, 0, 1, 1};
    meshes[1].emissive = true;
    const std::vector<std::uint8_t> bytes = sv::write_fbx(meshes, {{"Cam", {0, 2, 10}, {0, 2, 0}, 40}});
    REQUIRE(std::string(bytes.begin(), bytes.begin() + 18) == "Kaydara FBX Binary");

    const sv::FbxScene scene = sv::load_fbx(bytes);
    REQUIRE(scene.groups.size() == 2);
    CHECK(scene.groups[0].name == "Truss 1");
    CHECK(scene.groups[0].vertices.size() == 36 * 6);
    CHECK(scene.groups[0].colour[0] == Approx(0.25));
    CHECK(scene.groups[0].colour[2] == Approx(0.75));
    float lo = 1e9f, hi = -1e9f;
    for (std::size_t i = 1; i < scene.groups[0].vertices.size(); i += 6)
        lo = std::min(lo, scene.groups[0].vertices[i]), hi = std::max(hi, scene.groups[0].vertices[i]);
    CHECK(lo == Approx(5)); // metres and Y up survive the unit and axis conversion
    CHECK(hi == Approx(6));
    // Nodes: the Trusses and Walls nulls (cameras have no mesh) with their meshes below.
    REQUIRE(scene.nodes.size() == 4);
    CHECK(scene.nodes[0].name == "Trusses");
    CHECK(scene.nodes[1].parent == 0);
    CHECK(scene.nodes[1].group == 0);
    CHECK(scene.nodes[2].name == "Walls");
}

TEST_CASE("load_fbx rejects garbage") {
    CHECK_THROWS_AS(sv::load_fbx(std::vector<std::uint8_t>(64, 7)), sv::InterchangeError);
}

TEST_CASE("OBJ groups take their colour from the MTL usemtl (FR-3)") {
    const sv::ObjMaterials mats = sv::parse_mtl("newmtl Red\nKd 1 0 0\nnewmtl Plain\n");
    REQUIRE(mats.size() == 2);
    CHECK(mats.at("Plain")[0] == Approx(0.8));
    CHECK(sv::obj_mtllibs("mtllib set.mtl\r\nv 0 0 0\n") == std::vector<std::string>{"set.mtl"});
    const auto groups =
        sv::parse_obj("v 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl Red\ng a\nf 1 2 3\ng b\nusemtl Nope\nf 1 2 3\n", mats);
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].colour == std::array<float, 4>{1, 0, 0, 1});
    CHECK(groups[1].colour[3] == 0);
}
