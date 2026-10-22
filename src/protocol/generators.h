#pragma once
// Internal test sources (FR-11): pattern generators ported from the POC's runGenerator.
#include "protocol/constants.h"

#include <map>
#include <string>
#include <vector>

namespace sv::proto {

enum class Pattern { off, full, chase, rainbow, sweep };

struct GeneratorTarget {
    std::uint16_t universe = 1;
    std::uint16_t address = 1;                     // 1-based
    std::map<std::string, std::uint16_t> channels; // GDTF attribute -> 0-based offset in the footprint
    struct Cell {
        std::uint16_t r, g, b;
    };
    std::vector<Cell> cells; // pixel cells (bars); rainbow-spread by the generators
};

// Writes pattern values for time t (seconds) into frames[universe]; only the listed channels are touched.
void run_generator(Pattern pattern, double t, double speed, const std::vector<GeneratorTarget> &targets,
                   std::map<std::uint16_t, Slots> &frames);

} // namespace sv::proto
