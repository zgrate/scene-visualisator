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

TEST_CASE("patch CSV, grandMA3 XML and Resolume XML match the approved files for the demo rig") {
    const json project = demo_project();
    approve("demo_patch.csv", patch_csv(project));
    approve("demo_patch_ma3.xml", ma3_patch_xml(project));
    approve("demo_advanced_output.xml", resolume_xml(project));
}

TEST_CASE("CSV quotes names and leaves unpatched fixtures blank") {
    json p = new_project();
    json f = new_fixture("Spot, \"left\"", "Generic@Spot_7ch.gdtf", "Default");
    p["objects"].push_back(f);
    CHECK(patch_csv(p).ends_with("\n,\"Spot, \"\"left\"\"\",\"Generic@Spot_7ch\",\"Default\",,,,0,0,0,0,0,0\n"));
}

TEST_CASE("Resolume XML round trip restores walls matched by name") {
    json project = demo_project();
    const std::string xml = resolume_xml(project);
    json changed = project;
    for (json &o : changed["objects"])
        if (o["type"] == "ledWall")
            o["slice"] = {{"x", 0}, {"y", 0}, {"w", 1}, {"h", 1}};
    changed["objects"].push_back(new_led_wall("Not in the file"));
    const json patch = resolume_import_patch(changed, xml);
    const json back = changed.patch(patch);
    CHECK(back["objects"].size() == changed["objects"].size()); // all three matched, none added
    for (std::size_t i = 0; i < project["objects"].size(); ++i)
        CHECK(back["objects"][i] == project["objects"][i]);

    const json fresh = new_project().patch(resolume_import_patch(new_project(), xml));
    REQUIRE(fresh["objects"].size() == 3);
    CHECK(fresh["objects"][0]["name"] == "Centre wall");
    CHECK(fresh["objects"][0]["width"] == 6.0);
    CHECK(fresh["objects"][0]["slice"]["w"] == 1280);
    CHECK_THROWS_AS(resolume_import_patch(project, "<XmlState/>"), InterchangeError);
    CHECK_THROWS_AS(resolume_import_patch(project, "<broken"), InterchangeError);
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
