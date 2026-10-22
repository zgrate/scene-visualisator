#include "core/interchange.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <sstream>

using namespace sv;

namespace {

// Approval test: compares with tests/approved/<name>; on a mismatch writes <name>.received next to it.
void approve(const std::string &name, const std::string &actual) {
    const std::string path = SV_SOURCE_DIR "/tests/approved/" + name;
    std::ifstream in(path, std::ios::binary);
    std::stringstream approved;
    approved << in.rdbuf();
    if (approved.str() != actual)
        std::ofstream(path + ".received", std::ios::binary) << actual;
    INFO("approved file " << path << " differs, see .received");
    CHECK(approved.str() == actual);
}

} // namespace

TEST_CASE("patch CSV and grandMA3 XML match the approved files for the demo rig") {
    const json project = demo_project();
    approve("demo_patch.csv", patch_csv(project));
    approve("demo_patch_ma3.xml", ma3_patch_xml(project));
}

TEST_CASE("CSV quotes names and leaves unpatched fixtures blank") {
    json p = new_project();
    json f = new_fixture("Spot, \"left\"", "Generic@Spot_7ch.gdtf", "Default");
    p["objects"].push_back(f);
    CHECK(patch_csv(p).ends_with("\n,\"Spot, \"\"left\"\"\",\"Generic@Spot_7ch\",\"Default\",,,,0,0,0,0,0,0\n"));
}

TEST_CASE("OBJ parser fans polygons, splits groups and resolves negative indices") {
    const auto groups = parse_obj("# cube side\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\no Floor\nf 1 2 3 4\n"
                                  "g Tri\r\nf -4/1/1 -3//1 -2\n");
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].name == "Floor");
    CHECK(groups[0].vertices.size() == 2 * 3 * 6);
    CHECK(groups[0].vertices[5] == 1.0f); // CCW in XY: normal +Z
    CHECK(groups[1].name == "Tri");
    CHECK(groups[1].vertices.size() == 3 * 6);
    CHECK_THROWS_AS(parse_obj("v 0 0 0\nf 1 2 3\n"), InterchangeError);
    CHECK_THROWS_AS(parse_obj("v 0 0 0\n"), InterchangeError);
    CHECK(unit_scale("mm") == 0.001);
}
