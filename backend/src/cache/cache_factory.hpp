#pragma once

#include <memory>

#include "cache/cache_provider.hpp"

namespace monobucket {

struct Config;

/// Builds the cache the configuration asks for.
///
/// Never throws and never returns null. Everything a cache can fail at —
/// Redis being down, the binary having been built without Redis support — is a
/// reason to degrade, not a reason to refuse to start. The one thing that does
/// abort startup is a malformed `MONOBUCKET_REDIS_URL`, and that happens in
/// Config::validate() long before this is called, because a URL the operator
/// typed wrong is a mistake to report, not a condition to survive.
std::unique_ptr<CacheProvider> makeCache(const Config& config);

}  // namespace monobucket
