#include "protocol/generators.h"

#include <algorithm>
#include <cmath>

namespace sv::proto {

namespace {

std::array<double, 3> hsv(double h, double s, double v) {
    const double c = v * s, x = c * (1 - std::abs(std::fmod(h / 60, 2) - 1)), m = v - c;
    std::array<double, 3> rgb = h < 60    ? std::array{c, x, 0.0}
                                : h < 120 ? std::array{x, c, 0.0}
                                : h < 180 ? std::array{0.0, c, x}
                                : h < 240 ? std::array{0.0, x, c}
                                : h < 300 ? std::array{x, 0.0, c}
                                          : std::array{c, 0.0, x};
    for (double &k : rgb)
        k = (k + m) * 255;
    return rgb;
}

} // namespace

void smooth_colours(std::vector<std::array<double, 3>> &state, const std::vector<Rgb> &target, double dt_ms,
                    const GeneratorOptions &options, std::vector<Rgb> &out) {
    state.resize(target.size());
    out.resize(target.size());
    const double k = options.smooth_ms > 0 ? dt_ms / (options.smooth_ms + dt_ms) : 1.0;
    const double ceiling = 255.0 * std::clamp(options.max, 0.0, 1.0);
    for (std::size_t n = 0; n < target.size(); ++n)
        for (int c = 0; c < 3; ++c) {
            state[n][c] += (double(target[n][c]) - state[n][c]) * k;
            out[n][c] = std::uint8_t(std::lround(std::min(state[n][c], ceiling)));
        }
}

void run_generator(Pattern pattern, double t, double speed, const std::vector<GeneratorTarget> &targets,
                   std::map<std::uint16_t, Slots> &frames, std::span<const Rgb> colours) {
    if (pattern == Pattern::off)
        return;
    const auto n_fixtures = std::max<std::size_t>(targets.size(), 1);
    for (std::size_t n = 0; n < targets.size(); ++n) {
        const GeneratorTarget &f = targets[n];
        Slots &d = frames[f.universe];
        auto put = [&](std::size_t offset, double v) {
            const std::size_t slot = f.address - 1 + offset;
            if (slot < 512)
                d[slot] = std::uint8_t(std::clamp(std::lround(v), 0L, 255L));
        };
        auto set = [&](const char *attr, double v) {
            if (auto it = f.channels.find(attr); it != f.channels.end())
                put(it->second, v);
        };
        const double ph = t * speed + double(n) * 0.5, hue = std::fmod(ph * 40, 360);
        auto [r, g, b] = hsv(hue, 1, 1);
        if (pattern == Pattern::mapping) {
            const Rgb c = n < colours.size() ? colours[n] : Rgb{};
            r = c[0], g = c[1], b = c[2];
        }
        switch (pattern) {
        case Pattern::full:
            for (auto a : {"Dimmer", "ColorAdd_R", "ColorAdd_G", "ColorAdd_B", "ColorAdd_W"})
                set(a, 255);
            set("Pan", 128), set("Tilt", 128);
            break;
        case Pattern::chase:
            set("Dimmer", std::size_t(std::floor(ph * 2)) % n_fixtures == n % n_fixtures ? 255 : 20);
            set("ColorAdd_R", 255), set("ColorAdd_G", 120), set("ColorAdd_B", 40);
            set("Pan", 128), set("Tilt", 128);
            break;
        case Pattern::rainbow:
            set("Dimmer", 200 + 55 * std::sin(ph));
            set("ColorAdd_R", r), set("ColorAdd_G", g), set("ColorAdd_B", b);
            set("Color1", hue / 360 * 255);
            set("Pan", 128 + 40 * std::sin(ph * 0.3)), set("Tilt", 128 + 30 * std::cos(ph * 0.4));
            break;
        case Pattern::sweep:
            set("Dimmer", 255);
            set("ColorAdd_R", 255), set("ColorAdd_G", 255), set("ColorAdd_B", 255);
            set("Pan", 128 + 110 * std::sin(ph * 0.8)), set("Tilt", 128 + 90 * std::sin(ph * 0.5));
            set("Color1", 0);
            break;
        case Pattern::mapping:
            set("Dimmer", std::max({r, g, b}));
            set("ColorAdd_R", r), set("ColorAdd_G", g), set("ColorAdd_B", b), set("ColorAdd_W", std::min({r, g, b}));
            set("ColorSub_C", 255 - r), set("ColorSub_M", 255 - g), set("ColorSub_Y", 255 - b);
            set("Pan", 128), set("Tilt", 128);
            break;
        case Pattern::off:
            break;
        }
        for (std::size_t i = 0; i < f.cells.size(); ++i) {
            const auto [cr, cg, cb] =
                pattern == Pattern::mapping ? std::array{r, g, b} : hsv(std::fmod(hue + double(i) * 30, 360), 1, 1);
            put(f.cells[i].r, cr), put(f.cells[i].g, cg), put(f.cells[i].b, cb);
        }
    }
}

} // namespace sv::proto
