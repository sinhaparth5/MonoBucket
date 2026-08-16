#pragma once

#include <string>

namespace monobucket {
struct Config;
class CacheProvider;
class IoExecutor;
class StorageEngine;
}  // namespace monobucket

namespace monobucket::s3 {

struct S3Metrics;

/// Registers the S3 API on the catch-all route.
///
/// One handler covers both listeners. Drogon scans regex routes in registration
/// order and takes the first whose method binder exists, so two independent
/// catch-alls — one for the console, one for S3 — would shadow each other
/// depending on which was registered first. Dispatching on the listener port
/// inside a single handler removes the ordering from the equation entirely.
void registerS3Routes(const Config& config, StorageEngine& storage, IoExecutor& io,
                      CacheProvider& cache, S3Metrics& metrics);

/// The metrics block appended to /metrics.
std::string renderS3Metrics(const S3Metrics& metrics);

}  // namespace monobucket::s3
