#include "core/dmx_recording.h"
#include "core/model.h"
#include "core/units.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
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
          sv::new_truss("Truss A"), sv::new_set_piece("Riser", "geometry/riser.obj"), sv::new_camera("FOH"), sv::new_deck(4, 2, 1),
          sv::new_shape("Box", "box", 1, 1, 1)})
        for (const char *k : {"uuid", "type", "name", "parent", "position", "rotation", "scale"})
            CHECK(o.contains(k));
}

TEST_CASE("FR-2 schema 2 venue.decks migrate to deck objects (schema 3)") {
    sv::json p = sv::new_project();
    p["schemaVersion"] = 2;
    const sv::json truss = sv::new_truss("T");
    p["objects"] = {truss};
    p["venue"]["decks"] = {{{"uuid", "d1"}, {"position", {1, 0, 2}}, {"width", 6}, {"depth", 3}, {"height", 0.8}},
                           {{"uuid", "d2"}, {"position", {0, 0, 0}}, {"width", 2}, {"depth", 2}, {"height", 0.4}}};
    sv::migrate(p);
    CHECK(p["schemaVersion"] == sv::schema_version);
    CHECK_FALSE(p["venue"].contains("decks"));
    REQUIRE(p["objects"].size() == 3);
    CHECK(p["objects"][0]["type"] == "deck");
    CHECK(p["objects"][0]["uuid"] == "d1");
    CHECK(p["objects"][0]["width"] == 6);
    CHECK(p["objects"][0]["height"] == 0.8);
    CHECK(p["objects"][0]["position"] == sv::json{1, 0, 2});
    CHECK(p["objects"][1]["uuid"] == "d2");
    CHECK(p["objects"][2] == truss);
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

TEST_CASE("DMX recording keeps only changes, seeks across blocks and survives a truncated tail") {
    const auto path = (std::filesystem::temp_directory_path() / ("sv_rec_" + sv::new_uuid() + ".svdmx")).string();
    sv::DmxSlots a{}, b{};
    {
        sv::DmxRecordWriter w(path);
        for (std::uint32_t t = 0; t <= 3000; t += 25) { // 3 s at 40 Hz; universe 2 changes once
            a[0] = std::uint8_t(t / 25);
            w.write(t, 1, a);
            b[7] = t < 500 ? 10 : 99;
            w.write(t, 2, b);
        }
    }
    const auto size = std::filesystem::file_size(path);
    const sv::DmxRecording r = sv::DmxRecording::load(path);
    CHECK(r.duration_ms() == 3000);
    CHECK(r.state_at(0).at(1)[0] == 0);
    CHECK(r.state_at(0).at(2)[7] == 10);
    CHECK(r.state_at(510).at(1)[0] == 20);
    CHECK(r.state_at(2510).at(1)[0] == 100);
    CHECK(r.state_at(2510).at(2)[7] == 99); // from block 3's keyframe, written only once before
    CHECK(r.state_at(1000).at(2)[7] == 99);

    std::filesystem::resize_file(path, size - 5); // a crash mid-block drops only that block
    const sv::DmxRecording cut = sv::DmxRecording::load(path);
    CHECK(cut.duration_ms() < 3000);
    CHECK(cut.duration_ms() >= 1975);
    CHECK(cut.state_at(1500).at(1)[0] == 60);
    std::filesystem::remove(path);
    CHECK_THROWS(sv::DmxRecording::load(path));
}
