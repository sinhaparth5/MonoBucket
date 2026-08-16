#include "storage/durability.hpp"

namespace monobucket {

std::string_view toString(Durability durability) {
    switch (durability) {
        case Durability::None:    return "none";
        case Durability::Relaxed: return "relaxed";
        case Durability::Strict:  return "strict";
    }
    return "relaxed";
}

std::optional<Durability> durabilityFromString(std::string_view name) {
    if (name == "none") return Durability::None;
    if (name == "relaxed") return Durability::Relaxed;
    if (name == "strict") return Durability::Strict;
    return std::nullopt;
}

}  // namespace monobucket
