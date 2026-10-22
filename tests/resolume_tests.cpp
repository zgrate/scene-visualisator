#include "core/resolume.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <fstream>
#include <pugixml.hpp>
#include <set>
#include <sstream>

using namespace sv;
using Catch::Matchers::WithinAbs;

namespace {

std::string sample() {
    std::ifstream in(SV_SOURCE_DIR "/tests/fixtures/resolume/arena-7.17-futro-ajki26.xml", std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

json imported(const json &project = new_project()) {
    return project.patch(resolume_import(project, sample()).patch);
}

std::vector<json> of_type(const json &p, const char *type) {
    std::vector<json> out;
    for (const json &o : p["objects"])
        if (o["type"] == type)
            out.push_back(o);
    return out;
}

json &by_name(json &p, const std::string &name) {
    for (json &o : p["objects"])
        if (o["name"] == name)
            return o;
    throw std::runtime_error("no " + name);
}

} // namespace

TEST_CASE("FR-24 the Arena 7.17 show file imports to 4 walls on 2 outputs and 41 strips on 8 Lumiverses") {
    const auto result = resolume_import(new_project(), sample());
    const json p = new_project().patch(result.patch);
    CHECK(p["composition"]["width"] == 2500);
    CHECK(p["composition"]["height"] == 1080);
    int video = 0, lumiverses = 0;
    for (const json &o : p["outputs"])
        (o["kind"] == "lumiverse" ? lumiverses : video)++;
    CHECK(video == 2);
    CHECK(lumiverses == 8);
    CHECK(of_type(p, "ledWall").size() == 4);
    const auto strips = of_type(p, "pixelStrip");
    REQUIRE(strips.size() == 41);
    CHECK(p["outputs"][0]["kind"] == "ndi");
    CHECK(p["outputs"][1]["width"] == 1024);
    CHECK(p["outputs"][1]["height"] == 768);

    // The diamonds of the LINSN screen are rotated 45 degrees.
    int diamonds = 0;
    for (const json &w : of_type(p, "ledWall"))
        diamonds += std::abs(std::abs(w["slice"]["angle"].get<double>()) - 45) < 0.01;
    CHECK(diamonds == 2);
    CHECK(strips[0]["pixels"] == 90);
    CHECK(strips[0]["colorFormat"] == "rgb");
    CHECK(strips[0]["distribution"] == 170);
    // All eight Lumiverses send Net 0 Sub 0 Uni 0, so each gets its own project universes and a warning says so.
    std::set<int> bases;
    for (const json &o : p["outputs"])
        if (o["kind"] == "lumiverse")
            bases.insert(o["universe"].get<int>());
    CHECK(bases.size() == 8);
    bool warned_targets = false, warned_layer = false;
    for (const std::string &w : result.warnings)
        warned_targets |= w.find("same Art-Net target") != std::string::npos,
            warned_layer |= w.find("layer input 3:3") != std::string::npos;
    CHECK(warned_targets);
    CHECK(warned_layer);
}

TEST_CASE("FR-24 exporting an imported show gives Arena's file back byte for byte") {
    const json p = imported();
    const std::string out = resolume_export(p);
    if (out != sample())
        std::ofstream(SV_SOURCE_DIR "/tests/approved/arena-roundtrip.xml.received", std::ios::binary) << out;
    CHECK(out == sample());
    // Importing again matches everything by uniqueId: nothing is added.
    const json again = p.patch(resolume_import(p, sample()).patch);
    CHECK(again["objects"].size() == p["objects"].size());
    CHECK(again["outputs"].size() == p["outputs"].size());
}

TEST_CASE("B12 strip addresses follow Arena's Distribution rule on all 33 strip boundaries") {
    pugi::xml_document doc;
    const std::string xml = sample();
    REQUIRE(doc.load_buffer(xml.data(), xml.size()));
    int checked = 0;
    for (pugi::xpath_node screen : doc.select_nodes("//DmxScreen")) {
        std::vector<std::pair<int, int>> strips; // start channel, pixels
        for (pugi::xml_node s : screen.node().child("layers").children("DmxSlice"))
            strips.emplace_back(
                int(s.select_node("Params[@name='Input']/ParamRange[@name='Start Channel']").node().attribute("value").as_double()),
                int(s.select_node(".//ParamRange[@name='Width']").node().attribute("value").as_double()));
        for (std::size_t i = 0; i + 1 < strips.size(); ++i) {
            json strip = new_pixel_strip("s", strips[i].second);
            strip["patch"] = {{"universe", 1 + (strips[i].first - 1) / 512}, {"address", (strips[i].first - 1) % 512 + 1}};
            const DmxAddress last = strip_pixel_address(strip, strips[i].second - 1);
            const int next = (last.universe - 1) * 512 + last.address + 3; // channel after the last pixel
            const DmxAddress follow = strip_pixel_address(
                {{"patch", {{"universe", (next - 1) / 512 + 1}, {"address", (next - 1) % 512 + 1}}},
                 {"colorFormat", "rgb"},
                 {"distribution", 170}},
                0);
            CHECK((follow.universe - 1) * 512 + follow.address == strips[i + 1].first);
            ++checked;
        }
    }
    CHECK(checked == 33);

    json strip = new_pixel_strip("s", 400);
    strip["patch"] = {{"universe", 3}, {"address", 1}};
    CHECK(strip_pixel_address(strip, 169).universe == 3);
    CHECK(strip_pixel_address(strip, 169).address == 508);
    CHECK(strip_pixel_address(strip, 170).universe == 4);
    CHECK(strip_pixel_address(strip, 170).address == 1);
    strip["patch"]["address"] = 508; // one pixel fits under 510 channels, the second starts the next universe
    CHECK(strip_pixel_address(strip, 1).universe == 4);
    CHECK(strip_pixel_address(strip, 1).address == 1);
}

TEST_CASE("FR-24 every Arena param in the show file can be edited") {
    const json p = imported();
    std::set<std::string> editable{"Name", "Start Channel", "Width", "Height", "Color Format", "Distribution",
                                   "Gamma"}; // carried by our own fields
    const auto collect = [&](const json &o) {
        for (const ResolumeParam &rp : resolume_params(p, o))
            editable.insert(json::json_pointer("/" + rp.key).back()); // unescapes "~1"
    };
    for (const json &o : p["objects"])
        collect(o);
    for (const json &o : p["outputs"])
        collect(o);
    collect(p["composition"]);

    pugi::xml_document doc;
    const std::string xml = sample();
    REQUIRE(doc.load_buffer(xml.data(), xml.size()));
    std::set<std::string> names;
    for (pugi::xpath_node n : doc.select_nodes("//*[starts-with(name(), 'Param') and @value]"))
        names.insert(n.node().attribute("name").value());
    CHECK(names.size() >= 40);
    for (const std::string &n : names) {
        INFO(n);
        CHECK(editable.contains(n));
    }
    // Metadata comes from the file: range and default.
    const auto params = resolume_params(p, p["outputs"][2]);
    const auto ip = std::find_if(params.begin(), params.end(), [](auto &r) { return r.key.ends_with("TargetIP"); });
    REQUIRE(ip != params.end());
    CHECK(ip->value == "TT_DISABLED");
    const auto fr = std::find_if(params.begin(), params.end(), [](auto &r) { return r.key.ends_with("Framerate"); });
    REQUIRE(fr != params.end());
    CHECK(fr->max == 40);
}

TEST_CASE("FR-24 edits reach the exported file and survive a re-import") {
    json p = imported();
    json &strip = by_name(p, of_type(p, "pixelStrip")[3]["name"]);
    const std::string strip_name = strip["name"];
    strip["resolume"]["params"]["Output/Brightness"] = 0.5;
    strip["pixels"] = 77;
    for (json &o : p["outputs"])
        if (o["kind"] == "lumiverse") {
            o["resolume"]["params"]["OutputDevice/OutputDeviceDmx/DmxOutputParams/TargetIP"] = "192.168.1.50";
            break;
        }
    json &wall = by_name(p, of_type(p, "ledWall")[0]["name"]);
    wall["slice"]["x"] = wall["slice"]["x"].get<double>() + 10;
    wall["slice"]["angle"] = 30.0;
    wall["name"] = "Renamed wall";
    p["composition"]["resolume"]["params"]["SoftEdging/Soft Edge/Power"] = 3;

    const std::string out = resolume_export(p);
    CHECK(out != sample());
    CHECK(out.find("value=\"192.168.1.50\"") != std::string::npos);
    CHECK(out.find("value=\"Renamed wall\"") != std::string::npos);
    CHECK(out.find("\r\n") != std::string::npos);
    CHECK(out.find(" />") == std::string::npos);

    const json back = new_project().patch(resolume_import(new_project(), out).patch);
    const json &w = by_name(const_cast<json &>(back), "Renamed wall");
    CHECK_THAT(w["slice"]["x"].get<double>(), WithinAbs(wall["slice"]["x"].get<double>(), 1e-3));
    CHECK_THAT(w["slice"]["angle"].get<double>(), WithinAbs(30, 1e-3));
    bool found = false;
    for (const json &o : back["objects"])
        if (o["name"] == strip_name && o["pixels"] == 77) {
            found = true;
            CHECK(o["resolume"]["params"]["Output/Brightness"] == "0.5");
        }
    CHECK(found);
    // Phase follows the value: Brightness -1..1 at 0.5 is phase 0.75.
    CHECK(out.find("phase=\"0.75\"") != std::string::npos);

    // Deleting a strip removes its DmxSlice.
    json fewer = p;
    auto &objs = fewer["objects"];
    objs.erase(std::find_if(objs.begin(), objs.end(), [](const json &o) { return o["type"] == "pixelStrip"; }));
    const json back2 = new_project().patch(resolume_import(new_project(), resolume_export(fewer)).patch);
    CHECK(of_type(back2, "pixelStrip").size() == 40);
}

TEST_CASE("FR-24 a project built from scratch exports screens Arena's layout understands") {
    json p = new_project();
    json wall = new_led_wall("Centre");
    wall["slice"] = {{"x", 0}, {"y", 0}, {"w", 1024}, {"h", 640}, {"angle", 0}};
    p["objects"].push_back(wall);
    json strip = new_pixel_strip("Edge", 120);
    strip["patch"] = {{"universe", 1}, {"address", 1}};
    p["objects"].push_back(strip);

    const std::string out = resolume_export(p);
    const auto r = resolume_import(new_project(), out);
    const json back = new_project().patch(r.patch);
    REQUIRE(of_type(back, "ledWall").size() == 1);
    REQUIRE(of_type(back, "pixelStrip").size() == 1);
    CHECK(of_type(back, "ledWall")[0]["name"] == "Centre");
    CHECK_THAT(of_type(back, "ledWall")[0]["slice"]["w"].get<double>(), WithinAbs(1024, 1e-3));
    CHECK(of_type(back, "pixelStrip")[0]["pixels"] == 120);
    CHECK(back["outputs"].size() == 2);
    CHECK_THROWS_AS(resolume_import(p, "<XmlState/>"), InterchangeError);
    CHECK_THROWS_AS(resolume_import(p, "<broken"), InterchangeError);
}
