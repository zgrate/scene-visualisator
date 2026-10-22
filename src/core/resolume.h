#pragma once
// FR-24 Resolume Arena 7 Advanced Output (B3). Import turns Screens into `outputs` and their Slices into LED walls,
// DmxScreens (Lumiverses) into `lumiverse` outputs and their DmxSlices into pixel strips. The imported file is kept
// as `resolumeSource`, so export rewrites only what changed and an untouched project gives the file back byte for
// byte. Every Arena param an object carries is stored as Arena's text under `resolume.params`, keyed by path.
#include "core/interchange.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sv {

struct ResolumeImportOptions {
    double pixel_pitch = 3.9; // mm per composition pixel: sizes new walls and strips, and scales the placement
    bool place = true;        // place new objects where the composition draws them, read as a front elevation
    double base = 0;          // height of the composition's bottom edge, metres
    double plane_z = -4;      // depth of the elevation plane, metres
};

struct ResolumeImport {
    json patch; // for Document::apply
    std::vector<std::string> warnings;
};

// Matches outputs and objects by Arena uniqueId, then by name. Throws InterchangeError.
ResolumeImport resolume_import(const json &project, std::string_view xml, const ResolumeImportOptions &options = {});
std::string resolume_export(const json &project);

struct ResolumeParam {
    std::string key; // path inside the element, e.g. "Output/Opacity", "@LumiverseId"; "/" in a name is "~1"
    std::string element; // Param, ParamRange, ParamChoice or "attribute"
    std::string type;    // Arena's T: DOUBLE, BOOL, STRING, INT32, UINT8, or empty
    std::string value, def;
    std::optional<double> min, max;
};
// Every Arena param of an output, wall or pixel strip in file order, with the object's value where it has one.
// Pass project["composition"] (no uuid) for the global ScreenSetup params such as SoftEdging.
// Params our own fields model (names, pixel counts, start channel) are left out.
std::vector<ResolumeParam> resolume_params(const json &project, const json &object);

// A JSON value in Arena's text form: bools 1/0, numbers as %.20g, strings as they are.
std::string resolume_text(const json &value);

} // namespace sv
