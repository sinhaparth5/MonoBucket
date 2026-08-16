#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace monobucket::env {

/// Raised when an environment variable is present but unparseable. Startup
/// treats this as fatal: a misspelled limit must never silently fall back to a
/// default the operator did not ask for.
class ParseError : public std::runtime_error {
public:
    ParseError(std::string_view name, std::string_view raw, std::string_view expected);

    const std::string& variable() const noexcept { return variable_; }

private:
    std::string variable_;
};

/// Reads a variable, treating empty strings as unset.
std::optional<std::string> lookup(std::string_view name);

std::string      string(std::string_view name, std::string fallback);
bool             boolean(std::string_view name, bool fallback);
std::uint64_t    number(std::string_view name, std::uint64_t fallback);
std::uint64_t    bytes(std::string_view name, std::uint64_t fallback);

// --- Pure parsers ----------------------------------------------------------
// Exposed independently of the environment so they can be unit tested.

/// Accepts 1/0, true/false, yes/no, on/off (case-insensitive).
bool parseBoolean(std::string_view raw);

/// Accepts a plain unsigned decimal integer.
std::uint64_t parseNumber(std::string_view raw);

/// Accepts a size with an optional unit suffix, e.g. `512`, `64MiB`, `1.5G`,
/// `200MB`. Binary units (`K`, `Ki`, `KiB`, ...) are powers of 1024; SI units
/// (`KB`, `MB`, `GB`, ...) are powers of 1000.
std::uint64_t parseBytes(std::string_view raw);

/// Renders a byte count back into a compact binary string, e.g. `1.5 GiB`.
std::string formatBytes(std::uint64_t value);

}  // namespace monobucket::env
