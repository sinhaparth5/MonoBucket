#pragma once

#include <cstddef>

namespace monobucket {

struct Config;

/// /healthz, /readyz, /metrics and /_mb/version — registered on every listener.
void registerSystemRoutes(const Config& config);

/// Serves the embedded SvelteKit dashboard on the console listener. A no-op
/// when the binary was built without MONOBUCKET_EMBED_FRONTEND.
void registerConsoleRoutes(const Config& config);

/// Resident set size of this process in bytes, or 0 where it cannot be read.
std::size_t residentBytes() noexcept;

}  // namespace monobucket
