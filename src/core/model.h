#pragma once
// The project document is a JSON tree (project.json of the spec "Project file specification").
// These factories define the defaults for each entity of the spec "Data model"; keys follow the file spec.
#include <nlohmann/json.hpp>
#include <string>

namespace sv {

using json = nlohmann::json;

inline constexpr int schema_version = 2;

std::string new_uuid(); // random v4, lowercase 8-4-4-4-12

json new_project();
// Upgrades an older project.json in place to schema_version (unknown keys stay). Schema 1 is the alpha.
void migrate(json &project);
json new_deck(double width, double depth, double height); // stage deck or riser, positioned by "position"

// Scene objects. All share uuid, type, name, parent (uuid or null), position, rotation (deg), scale.
json new_fixture(std::string name, std::string gdtf_file, std::string mode);
json new_led_wall(std::string name);
json new_truss(std::string name, std::string truss_type = "straight", double length = 3.0);
json new_set_piece(std::string name, std::string geometry_file);
json new_camera(std::string name);
// B12 LED pixel strip (WLED / Resolume Lumiverse). Cells run along local +X over `length` metres, centred.
json new_pixel_strip(std::string name, int pixels = 60);

// Channels per pixel of a strip: the length of its colorFormat ("rgb" 3, "rgbw" 4).
int bytes_per_pixel(const json &strip);
struct DmxAddress {
    int universe, address;
};
// Arena's Distribution rule: `distribution` pixels fill a universe, a pixel never splits across two, and the next
// universe starts at channel 1. Pixel 0 sits at the strip's patch.
DmxAddress strip_pixel_address(const json &strip, int pixel);

// The POC demo rig: a stage deck, 2 trusses, 19 fixtures of the bundled generic types on universe 1, 3 LED walls.
json demo_project();

// FR-7 derived values of an LED wall.
struct WallDerived {
    int pixels_x, pixels_y;   // native resolution
    int modules_x, modules_y; // module count
};
WallDerived wall_derived(const json &wall);

} // namespace sv
