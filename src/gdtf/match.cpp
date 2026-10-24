#include "gdtf/match.h"

#include <algorithm>
#include <cctype>
#include <numeric>
#include <utility>
#include <vector>

namespace sv::gdtf {

namespace {

std::size_t edit_distance(std::string_view a, std::string_view b) {
    std::vector<std::size_t> row(b.size() + 1);
    std::iota(row.begin(), row.end(), 0);
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j)
            diagonal = std::exchange(row[j], std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] != b[j - 1])}));
    }
    return row.back();
}

std::vector<long> numbers(std::string_view s) {
    std::vector<long> out;
    for (std::size_t i = 0; i < s.size();) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
            ++i;
            continue;
        }
        long v = 0;
        for (; i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])); ++i)
            v = std::min(v * 10 + (s[i] - '0'), 1000000L);
        out.push_back(v);
    }
    return out;
}

} // namespace

std::string name_key(std::string_view text) {
    const auto lower = [](unsigned char c) { return char(std::tolower(c)); };
    if (text.size() >= 5 && std::ranges::equal(text.substr(text.size() - 5), std::string_view(".gdtf"), {}, lower))
        text.remove_suffix(5);
    std::string out;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x80 || std::isalnum(u))
            out += lower(u);
    }
    return out;
}

Match match_type(const std::map<std::string, FixtureType, std::less<>> &library, std::string_view text) {
    const std::string key = name_key(text);
    if (key.empty())
        return {};
    // "Manufacturer@Name@Version": the version never takes part.
    const std::size_t at = text.find('@');
    const std::string maker = at == std::string_view::npos ? std::string() : name_key(text.substr(0, at));
    std::string_view rest = at == std::string_view::npos ? text : text.substr(at + 1);
    rest = rest.substr(0, rest.find('@'));
    const std::string name = name_key(rest);
    // Ranked, so a file named exactly as asked beats a type whose bare name happens to spell "Maker Name".
    const auto rank = [&](const std::string &file, const FixtureType &t) {
        if (key == name_key(file))
            return 0;
        if (key == name_key(t.manufacturer + "@" + t.name) ||
            (!maker.empty() && maker == name_key(t.manufacturer) && name == name_key(t.name)))
            return 1;
        if (key == name_key(t.name) || (!t.short_name.empty() && key == name_key(t.short_name)))
            return 2;
        return 3;
    };
    const std::string *exact = nullptr;
    int exact_rank = 3;
    for (const auto &[file, t] : library)
        if (const int r = rank(file, t); r < exact_rank)
            exact = &file, exact_rank = r;
    if (exact)
        return {*exact, true};
    Match best;
    std::size_t best_distance = std::max<std::size_t>(2, name.size() / 3) + 1;
    for (const auto &[file, t] : library) {
        const std::string candidate = name_key(t.name);
        if ((!maker.empty() && maker != name_key(t.manufacturer)) || numbers(candidate) != numbers(name))
            continue;
        if (const std::size_t d = edit_distance(name, candidate); d < best_distance)
            best = {file, false}, best_distance = d;
    }
    return best;
}

Match match_mode(const FixtureType &type, std::string_view wanted) {
    if (type.modes.empty())
        return {};
    const std::string key = name_key(wanted);
    for (const Mode &m : type.modes)
        if (m.name == wanted || name_key(m.name) == key)
            return {m.name, true};
    const auto contains = [&](const Mode &m) {
        const std::string k = name_key(m.name);
        return !k.empty() && !key.empty() && (key.find(k) != std::string::npos || k.find(key) != std::string::npos);
    };
    const std::vector<long> n = numbers(wanted);
    const long channels = n.empty() ? 0 : *std::ranges::max_element(n);
    const Mode *sized = nullptr;
    for (const Mode &m : type.modes)
        if (m.footprint == channels && (!sized || (contains(m) && !contains(*sized))))
            sized = &m;
    if (sized)
        return {sized->name, false};
    for (const Mode &m : type.modes)
        if (contains(m))
            return {m.name, false};
    return {type.modes.front().name, false};
}

} // namespace sv::gdtf
