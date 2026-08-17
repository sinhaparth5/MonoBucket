#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <drogon/HttpResponse.h>

namespace monobucket {

struct Config;
class CacheProvider;
class IoExecutor;
class StorageEngine;

namespace s3 {
struct S3Metrics;
}

/// /healthz, /readyz, /metrics and /_mb/version — registered on every listener.
///
/// These are exact-path handlers, which Drogon matches before the catch-all
/// regex the S3 API is registered on. That is what makes them reachable at all,
/// and it is also why `healthz`, `readyz`, `metrics` and `_mb` are refused as
/// bucket names: a bucket with one of those names would exist and be
/// unaddressable.
void registerSystemRoutes(const Config& config, StorageEngine& storage, IoExecutor& io,
                          CacheProvider& cache, const s3::S3Metrics& metrics);

/// The embedded dashboard asset for a console path, or nullptr when the binary
/// carries no dashboard or the path does not resolve to one.
///
/// Returned rather than registered on its own route: Drogon takes the first
/// matching regex handler, so the console and the S3 API cannot each own a
/// catch-all. The S3 router calls this for requests arriving on the console
/// listener, passing the request's `Accept-Encoding` so a pre-compressed
/// variant can be chosen where one was generated.
drogon::HttpResponsePtr consoleAssetResponse(const std::string& path,
                                             std::string_view acceptEncoding);

/// Resident set size of this process in bytes, or 0 where it cannot be read.
std::size_t residentBytes() noexcept;

}  // namespace monobucket
