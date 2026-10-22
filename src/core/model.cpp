#include "core/model.h"

#include <cmath>
#include <cstdio>
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

WallDerived wall_derived(const json &wall) {
    const double w = wall.at("width"), h = wall.at("height"), pitch_m = wall.at("pixelPitch").get<double>() / 1000.0;
    const auto count = [](double size, double step) { return step > 0 ? int(std::lround(size / step)) : 0; };
    return {count(w, pitch_m), count(h, pitch_m), count(w, wall.at("moduleWidth")), count(h, wall.at("moduleHeight"))};
}

} // namespace sv
