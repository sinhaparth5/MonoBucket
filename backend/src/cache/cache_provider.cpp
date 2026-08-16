#include "cache/cache_provider.hpp"

namespace monobucket {

CacheValuePtr makeCacheValue(std::string value) {
    return std::make_shared<const std::string>(std::move(value));
}

double CacheStats::hitRatio() const noexcept {
    const std::uint64_t lookups = hits + misses;
    if (lookups == 0) return 0.0;
    return static_cast<double>(hits) / static_cast<double>(lookups);
}

void CacheProvider::put(std::string_view key, std::string_view value,
                        std::chrono::seconds ttl) {
    set(key, makeCacheValue(std::string(value)), ttl);
}

}  // namespace monobucket
