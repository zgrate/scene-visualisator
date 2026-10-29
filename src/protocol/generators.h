#pragma once
// Internal test sources (FR-11): pattern generators ported from the POC's runGenerator.
#include "protocol/constants.h"

#include <array>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace sv::proto {

// mapping: each fixture shows the composition colour at the nearest LED wall point (its colour comes in through
// DmxEngine::set_generator_colours).
enum class Pattern { off, full, chase, rainbow, sweep, mapping };
constexpr std::array<std::pair<const char *, Pattern>, 6> pattern_names{{{"off", Pattern::off},
                                                                         {"full", Pattern::full},
                                                                         {"chase", Pattern::chase},
                                                                         {"rainbow", Pattern::rainbow},
                                                                         {"sweep", Pattern::sweep},
                                                                         {"mapping", Pattern::mapping}}};
using Rgb = std::array<std::uint8_t, 3>;

// Pattern::mapping only: a transition between the LED colours and a ceiling, so cuts do not flash the rig.
struct GeneratorOptions {
    double smooth_ms = 150; // time constant of the per-channel transition; 0 is off
    double max = 1.0;       // 0..1 ceiling per channel
};
// One tick of the transition: state follows target with the time constant, out is the clipped state. state is
// resized to target (new entries start black).
void smooth_colours(std::vector<std::array<double, 3>> &state, const std::vector<Rgb> &target, double dt_ms,
                    const GeneratorOptions &options, std::vector<Rgb> &out);

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
// colours[n] is target n's mapping colour (missing = black).
void run_generator(Pattern pattern, double t, double speed, const std::vector<GeneratorTarget> &targets,
                   std::map<std::uint16_t, Slots> &frames, std::span<const Rgb> colours = {});

} // namespace sv::proto
