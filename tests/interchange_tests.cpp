#include "core/interchange.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <sstream>

using namespace sv;
using Catch::Approx;

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

TEST_CASE("CSV import reads our own patch CSV back and updates by fixture ID (FR-23)") {
    json project = demo_project();
    const auto rows = parse_csv(patch_csv(project));
    const CsvMapping m = guess_csv_mapping(rows.at(0));
    for (const char *f : csv_fields)
        CHECK(m.contains(f));
    // Same rig again: every row matches a fixture, so the patch only replaces (values unchanged).
    json patch = csv_import(project, rows, m, {});
    for (const json &op : patch)
        CHECK(op["op"] == "replace");
    json copy = project;
    copy = copy.patch(patch);
    CHECK(copy["objects"].size() == project["objects"].size());

    // New rows add fixtures, Z-up in millimetres; ';' with quotes and decimal commas.
    const auto vw = parse_csv("\xEF\xBB\xBFUnit Number;Instrument Type;DMX Mode;Absolute Address;X Location;Y "
                              "Location;Z Location\r\n901;\"Spot; big\";Default;520;1000;2000,5;3000\r\n;;;;;;\r\n");
    REQUIRE(vw.size() == 3); // the empty row is kept and skipped by csv_import
    const CsvMapping vm = guess_csv_mapping(vw[0]);
    CsvImportOptions o;
    o.scale = unit_scale("mm");
    o.z_up = true;
    o.resolve = [](const std::string &t, const std::string &mode) {
        return std::pair{t == "Spot; big" ? std::string("Generic@Spot_7ch.gdtf") : std::string(), mode};
    };
    patch = csv_import(project, vw, vm, o);
    REQUIRE(patch.size() == 1);
    const json &f = patch[0]["value"];
    CHECK(f["fixtureId"] == 901);
    CHECK(f["gdtf"] == "Generic@Spot_7ch.gdtf");
    CHECK(f["patch"] == json{{"universe", 2}, {"address", 8}});
    CHECK(f["position"][0].get<double>() == Approx(1.0));
    CHECK(f["position"][1].get<double>() == Approx(3.0));
    CHECK(f["position"][2].get<double>() == Approx(-2.0005));
}
