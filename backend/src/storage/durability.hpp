#pragma once

#include <optional>
#include <string_view>

namespace monobucket {

/// How much of a write must reach stable storage before it is acknowledged.
///
/// This is the one storage setting with no safe default for every deployment:
/// a throwaway CI container and a production volume want opposite answers, and
/// the cost difference is an fsync per operation.
enum class Durability {
    /// No explicit flushing. Survives a process crash — the kernel still owns
    /// the page cache and will write it back — but not a power cut.
    None,

    /// fsync each payload before publishing it. Survives a process crash, and
    /// the payload survives a power cut; the directory entry naming it may not.
    Relaxed,

    /// fsync the payload, the directory that names it, and the metadata
    /// write-ahead log on every commit. Survives a power cut.
    Strict,
};

std::string_view toString(Durability durability);

/// Parses `none` / `relaxed` / `strict`. Returns nothing for anything else, so
/// a misspelled setting fails startup rather than quietly weakening durability.
std::optional<Durability> durabilityFromString(std::string_view name);

}  // namespace monobucket
