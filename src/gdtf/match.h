#pragma once
// Type and mode text from other programs (MVR GDTFSpec, CSV, grandMA3) matched against loaded fixture types. The
// names rarely agree exactly: "Cameo@CL PixBar 650 Pro" is "PIXBAR 650 CPRO" on GDTF Share, "50 channel" is
// "50-CH-Mode".
#include "gdtf/fixture_type.h"

#include <map>

namespace sv::gdtf {

std::string name_key(std::string_view text); // lower-case ASCII letters, digits and non-ASCII bytes; ".gdtf" dropped

struct Match {
    std::string name; // library file name or mode name; empty when nothing fits
    bool exact = false;
};
// `text` is a file name, "Manufacturer@Name[@Version]" or a type name. Exact when the keys agree; otherwise the
// closest name by the same manufacturer with the same numbers, within a third of its length in edits.
Match match_type(const std::map<std::string, FixtureType, std::less<>> &library, std::string_view text);
// Exact when the keys agree; otherwise a mode whose footprint is the largest number in `wanted`, then a mode whose
// name contains or is contained in it, then the first mode.
Match match_mode(const FixtureType &type, std::string_view wanted);

} // namespace sv::gdtf
