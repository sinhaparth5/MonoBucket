#pragma once

namespace monobucket {

struct Config;
class StorageEngine;
class IoExecutor;
class CacheProvider;

namespace s3 {
struct S3Metrics;
}

/// Registers the console's JSON API under `/_mb/api/` and starts the sampler
/// that feeds its graphs.
///
/// The API answers only on the console listener. The S3 listener speaks one
/// protocol and gains nothing from a second one sharing its port — and a
/// dashboard endpoint reachable at `/_mb/api/...` on port 9000 would collide
/// with a bucket named `_mb`.
///
/// Must be called before the S3 catch-all is registered: Drogon matches
/// exact-path handlers ahead of regex ones, but only when they were registered
/// first.
void registerConsoleApi(const Config& config, StorageEngine& storage, IoExecutor& io,
                        CacheProvider& cache, const s3::S3Metrics& s3Metrics);

}  // namespace monobucket
