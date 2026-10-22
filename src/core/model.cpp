#include "core/model.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <random>

namespace sv {

std::string new_uuid() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    const std::uint64_t hi = (rng() & 0xFFFFFFFFFFFF0FFFull) | 0x4000ull;             // version 4
    const std::uint64_t lo = (rng() & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull; // variant 10
    char s[37];
    std::snprintf(s, sizeof s, "%08x-%04x-%04x-%04x-%012llx", unsigned(hi >> 32), unsigned((hi >> 16) & 0xFFFF),
                  unsigned(hi & 0xFFFF), unsigned(lo >> 48), static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFull));
    return s;
}

json new_project() {
    return {
        {"schemaVersion", schema_version},
        {"app", {{"name", "Scene Visualiser"}, {"version", "0.1.0"}}},
        {"units", "m"}, // UI display unit; numbers in the file are always metres
        {"venue",
         {{"width", 20.0}, {"depth", 15.0}, {"height", 8.0}, {"decks", json::array()}, {"geometry", json::array()}}},
        {"objects", json::array()},
        {"patch", {{"universes", json::array()}}},
        {"composition",
         {{"width", 1920}, {"height", 1080}, {"frameRate", 60}, {"source", "testPattern"}, {"layers", json::array()}}},
        {"layout", json::object()},
    };
}

json new_deck(double width, double depth, double height) {
    return {
        {"uuid", new_uuid()}, {"position", {0.0, 0.0, 0.0}}, {"width", width}, {"depth", depth}, {"height", height}};
}

static json new_object(const char *type, std::string name) {
    return {{"uuid", new_uuid()},          {"type", type},
            {"name", std::move(name)},     {"parent", nullptr},
            {"position", {0.0, 0.0, 0.0}}, {"rotation", {0.0, 0.0, 0.0}},
            {"scale", {1.0, 1.0, 1.0}}};
}

json new_fixture(std::string name, std::string gdtf_file, std::string mode) {
    json o = new_object("fixture", std::move(name));
    o["fixtureId"] = nullptr; // MA-style fixture ID, unique when set
    o["gdtf"] = std::move(gdtf_file);
    o["mode"] = std::move(mode);
    o["patch"] = nullptr;         // {"universe": u, "address": a} once patched
    o["limits"] = json::object(); // physical limits override, e.g. {"panMin": -170}
    o["housingColor"] = "#202020";
    return o;
}

json new_led_wall(std::string name) {
    json o = new_object("ledWall", std::move(name));
    o["width"] = 4.0;
    o["height"] = 2.5;
    o["pixelPitch"] = 3.9; // mm
    o["moduleWidth"] = 0.5;
    o["moduleHeight"] = 0.5;
    o["nits"] = 1500;
    o["gamma"] = 2.2;
    o["curvature"] = 0.0;                                       // degrees of arc across the width
    o["slice"] = {{"x", 0}, {"y", 0}, {"w", 1024}, {"h", 640}}; // input rect in composition pixels
    return o;
}

json new_truss(std::string name, std::string truss_type, double length) {
    json o = new_object("truss", std::move(name));
    o["trussType"] = std::move(truss_type); // straight | corner | circle
    o["length"] = length;
    return o;
}

json new_set_piece(std::string name, std::string geometry_file) {
    json o = new_object("setPiece", std::move(name));
    o["geometry"] = std::move(geometry_file); // path inside the package, e.g. geometry/stage.obj
    return o;
}

json new_camera(std::string name) {
    json o = new_object("camera", std::move(name));
    o["position"] = {0.0, 1.7, 15.0};
    o["target"] = {0.0, 2.0, 0.0};
    o["fov"] = 50.0;
    return o;
}

json demo_project() {
    json p = new_project();
    p["venue"]["width"] = 16.0, p["venue"]["depth"] = 12.0;
    json deck = new_deck(10, 6, 1);
    p["venue"]["decks"].push_back(deck);
    auto &objects = p["objects"];
    auto truss = [&](const char *name, double z) {
        json t = new_truss(name, "straight", 10);
        t["position"] = {0.0, 6.5, z};
        objects.push_back(t);
    };
    truss("Front truss", 1.5);
    truss("Back truss", -2.5);
    int address = 1, id = 101;
    std::map<std::string, int> count; // "Spot 1", "Spot 2", ...
    // Footprints of the bundled generic types (resources/gdtf), all in mode "Default".
    auto put = [&](const char *gdtf, const char *name, int footprint, double x, double y, double z, double tilt) {
        json f = new_fixture(std::string(name) + " " + std::to_string(++count[name]), gdtf, "Default");
        f["position"] = {std::round(x * 100) / 100, y, z};
        f["rotation"] = {tilt, 0.0, 0.0};
        f["fixtureId"] = id++;
        f["patch"] = {{"universe", 1}, {"address", address}};
        address += footprint;
        objects.push_back(f);
    };
    for (int i = 0; i < 6; ++i)
        put("Generic@Spot_7ch.gdtf", "Spot", 7, -4.5 + i * 1.8, 6.2, -2.5, 0);
    for (int i = 0; i < 6; ++i)
        put("Generic@Wash_RGBW.gdtf", "Wash", 5, -4.5 + i * 1.8, 6.3, 1.5,
            35); // +X rotation aims the beam upstage (-Z)
    for (int i = 0; i < 4; ++i)
        put("Generic@Beam_5ch.gdtf", "Beam", 5, -3 + i * 2, 1.5, -2.9, 180); // floor-standing, pointing up
    for (int i = 0; i < 2; ++i)
        put("Generic@Blinder.gdtf", "Blinder", 1, -2 + i * 4, 6.6, 1.5, -70);
    put("Generic@Pixel_Bar_8.gdtf", "Pixel bar", 25, 0, 1.15, -2.6, 0);
    objects.back()["rotation"] = {180.0, 0.0, 0.0};
    auto wall = [&](const char *name, double w, double h, double x, double z, double pan, int sx, int sw) {
        json o = new_led_wall(name);
        o["width"] = w, o["height"] = h;
        o["position"] = {x, 1.0, z};
        o["rotation"] = {0.0, pan, 0.0};
        o["slice"] = {{"x", sx}, {"y", 0}, {"w", sw}, {"h", 1080}};
        objects.push_back(o);
    };
    wall("Centre wall", 6, 3.5, 0, -2.7, 0, 0, 1280);
    wall("SR pillar", 1.5, 4, -5, -1.5, 30, 1280, 320);
    wall("SL pillar", 1.5, 4, 5, -1.5, -30, 1600, 320);
    return p;
}

WallDerived wall_derived(const json &wall) {
    const double w = wall.at("width"), h = wall.at("height"), pitch_m = wall.at("pixelPitch").get<double>() / 1000.0;
    const auto count = [](double size, double step) { return step > 0 ? int(std::lround(size / step)) : 0; };
    return {count(w, pitch_m), count(h, pitch_m), count(w, wall.at("moduleWidth")), count(h, wall.at("moduleHeight"))};
}

} // namespace sv
