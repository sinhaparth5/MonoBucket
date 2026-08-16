#include "server/system_routes.hpp"

#include <fstream>
#include <sstream>
#include <string>

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "cache/cache_provider.hpp"
#include "core/config.hpp"
#include "core/io_executor.hpp"
#include "monobucket/version.hpp"
#include "s3/metrics.hpp"
#include "s3/router.hpp"
#include "server/asset_store.hpp"
#include "server/server.hpp"
#include "storage/storage_engine.hpp"

namespace monobucket {
namespace {

using drogon::HttpRequestPtr;
using drogon::HttpResponse;
using drogon::HttpResponsePtr;
using ResponseCallback = std::function<void(const HttpResponsePtr&)>;

HttpResponsePtr jsonResponse(const nlohmann::json& body,
                             drogon::HttpStatusCode status = drogon::k200OK) {
    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(status);
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    resp->setBody(body.dump());
    return resp;
}

HttpResponsePtr textResponse(std::string body, const char* contentType) {
    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeString(contentType);
    resp->setBody(std::move(body));
    return resp;
}

/// True when the request arrived on the console listener rather than the S3 one.
bool onConsoleListener(const HttpRequestPtr& req, const Config& config) {
    return req->getLocalAddr().toPort() == config.consolePort;
}

std::string renderMetrics(const Config& config, StorageEngine& storage, IoExecutor& io,
                          CacheProvider& cache, const s3::S3Metrics& s3Metrics) {
    std::ostringstream os;

    os << "# HELP monobucket_build_info Build metadata; always 1.\n"
       << "# TYPE monobucket_build_info gauge\n"
       << "monobucket_build_info{version=\"" << version::kVersion << "\",region=\"" << config.region
       << "\",cache=\"" << toString(config.cacheBackend) << "\"} 1\n";

    os << "# HELP monobucket_uptime_seconds Seconds since the server started.\n"
       << "# TYPE monobucket_uptime_seconds counter\n"
       << "monobucket_uptime_seconds " << uptimeSeconds() << '\n';

    os << "# HELP monobucket_process_resident_bytes Resident set size of the server process.\n"
       << "# TYPE monobucket_process_resident_bytes gauge\n"
       << "monobucket_process_resident_bytes " << residentBytes() << '\n';

    os << "# HELP monobucket_worker_threads Configured event-loop worker threads.\n"
       << "# TYPE monobucket_worker_threads gauge\n"
       << "monobucket_worker_threads " << config.workerThreads << '\n';

    os << "# HELP monobucket_embedded_asset_bytes Size of the dashboard baked into the binary.\n"
       << "# TYPE monobucket_embedded_asset_bytes gauge\n"
       << "monobucket_embedded_asset_bytes " << assets::totalBytes() << '\n';

    const auto stats = storage.stats();

    os << "# HELP monobucket_buckets Buckets currently defined.\n"
       << "# TYPE monobucket_buckets gauge\n"
       << "monobucket_buckets " << stats.usage.buckets << '\n';

    os << "# HELP monobucket_objects Objects currently stored.\n"
       << "# TYPE monobucket_objects gauge\n"
       << "monobucket_objects " << stats.usage.objects << '\n';

    os << "# HELP monobucket_object_bytes Total size of stored object payloads.\n"
       << "# TYPE monobucket_object_bytes gauge\n"
       << "monobucket_object_bytes " << stats.usage.bytes << '\n';

    os << "# HELP monobucket_multipart_uploads Multipart uploads in progress.\n"
       << "# TYPE monobucket_multipart_uploads gauge\n"
       << "monobucket_multipart_uploads " << stats.usage.uploads << '\n';

    // A number that only ever climbs means reclamation has stalled, which is
    // the difference between "the disk is full" and "we are leaking".
    os << "# HELP monobucket_orphan_payloads Payloads awaiting reclamation.\n"
       << "# TYPE monobucket_orphan_payloads gauge\n"
       << "monobucket_orphan_payloads " << stats.usage.orphanBlobs << '\n';

    os << "# HELP monobucket_disk_total_bytes Capacity of the filesystem holding the data directory.\n"
       << "# TYPE monobucket_disk_total_bytes gauge\n"
       << "monobucket_disk_total_bytes " << stats.space.totalBytes << '\n';

    os << "# HELP monobucket_disk_available_bytes Space available to the server on that filesystem.\n"
       << "# TYPE monobucket_disk_available_bytes gauge\n"
       << "monobucket_disk_available_bytes " << stats.space.availableBytes << '\n';

    // The metadata engine's own memory, which is the part of RSS that is not
    // ours to shrink directly — only to budget.
    for (const auto& [name, value] : stats.engineGauges) {
        os << "# TYPE monobucket_metadata_" << name << " gauge\n"
           << "monobucket_metadata_" << name << "{engine=\"" << stats.engine << "\"} " << value
           << '\n';
    }

    const auto ioStats = io.stats();

    os << "# HELP monobucket_io_queue_depth Storage operations waiting for an I/O thread.\n"
       << "# TYPE monobucket_io_queue_depth gauge\n"
       << "monobucket_io_queue_depth " << ioStats.queued << '\n';

    os << "# HELP monobucket_io_active Storage operations executing right now.\n"
       << "# TYPE monobucket_io_active gauge\n"
       << "monobucket_io_active " << ioStats.active << '\n';

    os << "# HELP monobucket_io_completed_total Storage operations completed.\n"
       << "# TYPE monobucket_io_completed_total counter\n"
       << "monobucket_io_completed_total " << ioStats.completed << '\n';

    // Non-zero means load was shed because the disk could not keep up.
    os << "# HELP monobucket_io_rejected_total Storage operations refused because the queue was full.\n"
       << "# TYPE monobucket_io_rejected_total counter\n"
       << "monobucket_io_rejected_total " << ioStats.rejected << '\n';

    const auto cacheStats = cache.stats();
    const std::string backend(cache.name());

    // The budget this process holds, which is not always MONOBUCKET_CACHE_MAX_
    // BYTES: with a shared backend most of the budget lives in Redis and this
    // reports the local tier, which is the part that counts against our RSS.
    os << "# HELP monobucket_cache_limit_bytes Cache budget held in this process.\n"
       << "# TYPE monobucket_cache_limit_bytes gauge\n"
       << "monobucket_cache_limit_bytes{backend=\"" << backend << "\"} "
       << cacheStats.limitBytes << '\n';

    os << "# HELP monobucket_cache_bytes Bytes currently held, including per-entry overhead.\n"
       << "# TYPE monobucket_cache_bytes gauge\n"
       << "monobucket_cache_bytes{backend=\"" << backend << "\"} " << cacheStats.bytes << '\n';

    os << "# HELP monobucket_cache_entries Entries currently held.\n"
       << "# TYPE monobucket_cache_entries gauge\n"
       << "monobucket_cache_entries{backend=\"" << backend << "\"} " << cacheStats.entries << '\n';

    os << "# HELP monobucket_cache_hits_total Lookups answered from the cache.\n"
       << "# TYPE monobucket_cache_hits_total counter\n"
       << "monobucket_cache_hits_total{backend=\"" << backend << "\"} " << cacheStats.hits << '\n';

    os << "# HELP monobucket_cache_misses_total Lookups that had to go to storage.\n"
       << "# TYPE monobucket_cache_misses_total counter\n"
       << "monobucket_cache_misses_total{backend=\"" << backend << "\"} " << cacheStats.misses
       << '\n';

    // Derived here as well as being derivable by the scraper: it is the first
    // number anyone looks at, and a dashboard should not have to compute it.
    os << "# HELP monobucket_cache_hit_ratio Hits over lookups since startup.\n"
       << "# TYPE monobucket_cache_hit_ratio gauge\n"
       << "monobucket_cache_hit_ratio{backend=\"" << backend << "\"} " << cacheStats.hitRatio()
       << '\n';

    // Rising steadily means the budget is too small for the working set.
    os << "# HELP monobucket_cache_evictions_total Entries dropped to stay inside the budget.\n"
       << "# TYPE monobucket_cache_evictions_total counter\n"
       << "monobucket_cache_evictions_total{backend=\"" << backend << "\"} "
       << cacheStats.evictions << '\n';

    os << "# HELP monobucket_cache_expirations_total Entries dropped because their TTL passed.\n"
       << "# TYPE monobucket_cache_expirations_total counter\n"
       << "monobucket_cache_expirations_total{backend=\"" << backend << "\"} "
       << cacheStats.expirations << '\n';

    // Non-zero means single values are larger than a whole shard's budget, so
    // the cache is silently doing nothing for them.
    os << "# HELP monobucket_cache_rejections_total Values refused for exceeding the budget.\n"
       << "# TYPE monobucket_cache_rejections_total counter\n"
       << "monobucket_cache_rejections_total{backend=\"" << backend << "\"} "
       << cacheStats.rejections << '\n';

    os << "# HELP monobucket_cache_errors_total Failures talking to a remote cache backend.\n"
       << "# TYPE monobucket_cache_errors_total counter\n"
       << "monobucket_cache_errors_total{backend=\"" << backend << "\"} " << cacheStats.errors
       << '\n';

    // 0 while a remote backend is being bypassed. Requests still succeed; they
    // are just being served by the local tier, which is worth alerting on.
    os << "# HELP monobucket_cache_healthy Whether the cache backend is fully available.\n"
       << "# TYPE monobucket_cache_healthy gauge\n"
       << "monobucket_cache_healthy{backend=\"" << backend << "\"} "
       << (cacheStats.healthy ? 1 : 0) << '\n';

    os << s3::renderS3Metrics(s3Metrics);
    return os.str();
}

}  // namespace

std::size_t residentBytes() noexcept {
#if defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    if (!statm) return 0;

    std::size_t totalPages = 0;
    std::size_t residentPages = 0;
    if (!(statm >> totalPages >> residentPages)) return 0;

    const long pageSize = ::sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) return 0;
    return residentPages * static_cast<std::size_t>(pageSize);
#else
    return 0;
#endif
}

void registerSystemRoutes(const Config& config, StorageEngine& storage, IoExecutor& io,
                          CacheProvider& cache, const s3::S3Metrics& s3Metrics) {
    auto& app = drogon::app();

    // Liveness: answers as long as the event loop is turning.
    app.registerHandler(
        "/healthz",
        [](const HttpRequestPtr&, ResponseCallback&& callback) {
            callback(jsonResponse({{"status", "ok"}, {"version", version::kVersion}}));
        },
        {drogon::Get, drogon::Head});

    // Readiness: actually touches the storage engine, so a data directory that
    // has become unreadable takes the container out of rotation instead of
    // silently failing every request.
    app.registerHandler(
        "/readyz",
        [&config, &storage, &cache](const HttpRequestPtr&, ResponseCallback&& callback) {
            try {
                const auto stats      = storage.stats();
                const auto cacheStats = cache.stats();
                callback(jsonResponse({
                    {"status", "ready"},
                    {"dataDir", config.dataDir},
                    {"workerThreads", config.workerThreads},
                    {"engine", stats.engine},
                    {"buckets", stats.usage.buckets},
                    {"objects", stats.usage.objects},
                    {"diskAvailableBytes", stats.space.availableBytes},
                    // Reported, but never a reason to answer 503: a degraded
                    // cache is not a reason to take the container out of
                    // rotation, which is the whole point of the fallback.
                    {"cache", nlohmann::json{{"backend", std::string(cache.name())},
                                             {"healthy", cacheStats.healthy},
                                             {"entries", cacheStats.entries},
                                             {"bytes", cacheStats.bytes}}},
                }));
            } catch (const std::exception& ex) {
                callback(jsonResponse({{"status", "unavailable"}, {"error", ex.what()}},
                                      drogon::k503ServiceUnavailable));
            }
        },
        {drogon::Get, drogon::Head});

    app.registerHandler(
        "/_mb/version",
        [](const HttpRequestPtr&, ResponseCallback&& callback) {
            callback(jsonResponse({
                {"name", "MonoBucket"},
                {"version", version::kVersion},
                {"calver", nlohmann::json{{"year", version::kYear},
                                          {"month", version::kMonth},
                                          {"micro", version::kMicro}}},
                {"dashboardEmbedded", assets::embedded()},
            }));
        },
        {drogon::Get});

    if (config.metricsEnabled) {
        app.registerHandler(
            "/metrics",
            [&config, &storage, &io, &cache, &s3Metrics](const HttpRequestPtr&,
                                                         ResponseCallback&& callback) {
                callback(textResponse(renderMetrics(config, storage, io, cache, s3Metrics),
                                      "text/plain; version=0.0.4"));
            },
            {drogon::Get});
    }

    // Read-only view of the effective configuration for the settings panel.
    app.registerHandler(
        "/_mb/config",
        [&config](const HttpRequestPtr& req, ResponseCallback&& callback) {
            if (!onConsoleListener(req, config)) {
                callback(jsonResponse({{"error", "not found"}}, drogon::k404NotFound));
                return;
            }
            callback(jsonResponse(config.toJson()));
        },
        {drogon::Get});
}

drogon::HttpResponsePtr consoleAssetResponse(const std::string& path) {
    if (!assets::embedded()) return nullptr;

    const assets::Asset* asset = assets::find(path);
    if (asset == nullptr && (path == "/" || path.find('.') == std::string::npos)) {
        // Client-side routing: unknown extension-less paths get the SPA shell
        // and let SvelteKit resolve the route.
        asset = assets::indexDocument();
    }
    if (asset == nullptr) return nullptr;

    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k200OK);
    resp->setContentTypeString(std::string(asset->mime));
    resp->setBody(std::string(reinterpret_cast<const char*>(asset->data), asset->size));

    // SvelteKit fingerprints everything under /_app/immutable/.
    if (path.rfind("/_app/immutable/", 0) == 0) {
        resp->addHeader("Cache-Control", "public, max-age=31536000, immutable");
    } else {
        resp->addHeader("Cache-Control", "no-cache");
    }
    return resp;
}

}  // namespace monobucket
