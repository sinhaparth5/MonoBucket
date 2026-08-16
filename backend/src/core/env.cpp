#include "core/env.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>

namespace monobucket::env {
namespace {

std::string toLower(std::string_view in) {
    std::string out(in);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view trim(std::string_view in) {
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!in.empty() && isSpace(static_cast<unsigned char>(in.front()))) in.remove_prefix(1);
    while (!in.empty() && isSpace(static_cast<unsigned char>(in.back()))) in.remove_suffix(1);
    return in;
}

struct Unit {
    std::string_view suffix;
    double           multiplier;
};

// Longest suffixes first so `kib` is matched before `k`.
constexpr std::array<Unit, 13> kUnits{{
    {"kib", 1024.0},
    {"mib", 1024.0 * 1024},
    {"gib", 1024.0 * 1024 * 1024},
    {"tib", 1024.0 * 1024 * 1024 * 1024},
    {"kb", 1000.0},
    {"mb", 1000.0 * 1000},
    {"gb", 1000.0 * 1000 * 1000},
    {"tb", 1000.0 * 1000 * 1000 * 1000},
    {"k", 1024.0},
    {"m", 1024.0 * 1024},
    {"g", 1024.0 * 1024 * 1024},
    {"t", 1024.0 * 1024 * 1024 * 1024},
    {"b", 1.0},
}};

}  // namespace

ParseError::ParseError(std::string_view name, std::string_view raw, std::string_view expected)
    : std::runtime_error(std::string(name) + "='" + std::string(raw) + "' is not valid: expected " +
                         std::string(expected)),
      variable_(name) {}

std::optional<std::string> lookup(std::string_view name) {
    const char* raw = std::getenv(std::string(name).c_str());
    if (raw == nullptr) return std::nullopt;
    std::string_view view = trim(raw);
    if (view.empty()) return std::nullopt;
    return std::string(view);
}

std::string string(std::string_view name, std::string fallback) {
    if (auto raw = lookup(name)) return *raw;
    return fallback;
}

bool boolean(std::string_view name, bool fallback) {
    auto raw = lookup(name);
    if (!raw) return fallback;
    try {
        return parseBoolean(*raw);
    } catch (const std::invalid_argument&) {
        throw ParseError(name, *raw, "one of true/false, yes/no, on/off, 1/0");
    }
}

std::uint64_t number(std::string_view name, std::uint64_t fallback) {
    auto raw = lookup(name);
    if (!raw) return fallback;
    try {
        return parseNumber(*raw);
    } catch (const std::invalid_argument&) {
        throw ParseError(name, *raw, "an unsigned integer");
    }
}

std::uint64_t bytes(std::string_view name, std::uint64_t fallback) {
    auto raw = lookup(name);
    if (!raw) return fallback;
    try {
        return parseBytes(*raw);
    } catch (const std::invalid_argument&) {
        throw ParseError(name, *raw, "a byte size such as 512, 64MiB, 1.5G or 200MB");
    }
}

bool parseBoolean(std::string_view raw) {
    const std::string value = toLower(trim(raw));
    if (value == "1" || value == "true" || value == "yes" || value == "on") return true;
    if (value == "0" || value == "false" || value == "no" || value == "off") return false;
    throw std::invalid_argument("not a boolean");
}

std::uint64_t parseNumber(std::string_view raw) {
    const std::string value(trim(raw));
    if (value.empty()) throw std::invalid_argument("empty");
    if (!std::all_of(value.begin(), value.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; })) {
        throw std::invalid_argument("not a number");
    }
    try {
        return std::stoull(value);
    } catch (const std::exception&) {
        throw std::invalid_argument("out of range");
    }
}

std::uint64_t parseBytes(std::string_view raw) {
    const std::string value = toLower(trim(raw));
    if (value.empty()) throw std::invalid_argument("empty");

    std::size_t split = 0;
    while (split < value.size() &&
           (std::isdigit(static_cast<unsigned char>(value[split])) != 0 || value[split] == '.')) {
        ++split;
    }
    if (split == 0) throw std::invalid_argument("missing magnitude");

    const std::string mantissa = value.substr(0, split);
    if (std::count(mantissa.begin(), mantissa.end(), '.') > 1) {
        throw std::invalid_argument("malformed number");
    }

    double magnitude = 0.0;
    try {
        magnitude = std::stod(mantissa);
    } catch (const std::exception&) {
        throw std::invalid_argument("malformed number");
    }
    if (magnitude < 0.0 || !std::isfinite(magnitude)) throw std::invalid_argument("out of range");

    std::string suffix = value.substr(split);
    // Tolerate "64 MiB".
    suffix = std::string(trim(suffix));

    double multiplier = 1.0;
    if (!suffix.empty()) {
        // `ki`, `mi`, ... are accepted as shorthand for `kib`, `mib`, ...
        if (suffix.size() == 2 && suffix[1] == 'i') suffix.push_back('b');

        const auto unit = std::find_if(kUnits.begin(), kUnits.end(),
                                       [&](const Unit& u) { return u.suffix == suffix; });
        if (unit == kUnits.end()) throw std::invalid_argument("unknown unit");
        multiplier = unit->multiplier;
    }

    const double result = magnitude * multiplier;
    if (result > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        throw std::invalid_argument("out of range");
    }
    return static_cast<std::uint64_t>(result);
}

std::string formatBytes(std::uint64_t value) {
    constexpr std::array<const char*, 5> kNames{"B", "KiB", "MiB", "GiB", "TiB"};

    auto scaled = static_cast<double>(value);
    std::size_t index = 0;
    while (scaled >= 1024.0 && index + 1 < kNames.size()) {
        scaled /= 1024.0;
        ++index;
    }

    std::ostringstream os;
    os << std::fixed << std::setprecision(index == 0 ? 0 : 1) << scaled << ' ' << kNames[index];
    return os.str();
}

}  // namespace monobucket::env
