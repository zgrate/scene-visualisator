#include "core/model.h"
#include "core/units.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <regex>
#include <set>

TEST_CASE("degrees and radians round trip") {
    CHECK(sv::to_degrees(sv::to_radians(270.0)) == Catch::Approx(270.0));
    CHECK(sv::to_radians(180.0) == Catch::Approx(3.14159265358979));
}

TEST_CASE("FR-2 feet and metres convert both ways") {
    CHECK(sv::feet_to_metres(10.0) == Catch::Approx(3.048));
    CHECK(sv::metres_to_feet(sv::feet_to_metres(42.0)) == Catch::Approx(42.0));
}

TEST_CASE("UUIDs are v4 formatted and unique") {
    const std::regex v4("[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}");
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i) {
        const std::string u = sv::new_uuid();
        REQUIRE(std::regex_match(u, v4));
        seen.insert(u);
    }
    CHECK(seen.size() == 1000);
}

TEST_CASE("new project has the keys of the project file spec") {
    const sv::json p = sv::new_project();
    CHECK(p["schemaVersion"] == sv::schema_version);
    for (const char *k : {"app", "units", "venue", "objects", "patch", "composition", "layout"})
        CHECK(p.contains(k));
}

TEST_CASE("every scene object carries the common fields") {
    for (const sv::json &o :
         {sv::new_fixture("Spot 1", "Generic@Spot@1.gdtf", "Standard"), sv::new_led_wall("Main"),
          sv::new_truss("Truss A"), sv::new_set_piece("Riser", "geometry/riser.obj"), sv::new_camera("FOH")})
        for (const char *k : {"uuid", "type", "name", "parent", "position", "rotation", "scale"})
            CHECK(o.contains(k));
}

TEST_CASE("FR-7 LED wall derives native resolution and module count") {
    sv::json wall = sv::new_led_wall("Main");
    wall["width"] = 6.0;
    wall["height"] = 3.5;
    wall["pixelPitch"] = 3.9;
    const sv::WallDerived d = sv::wall_derived(wall);
    CHECK(d.pixels_x == 1538); // 6 / 0.0039 = 1538.46
    CHECK(d.pixels_y == 897);
    CHECK(d.modules_x == 12);
    CHECK(d.modules_y == 7);
}
