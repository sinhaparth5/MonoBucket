#include "server/system_routes.hpp"

#include <fstream>
#include <sstream>
#include <string>

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "core/config.hpp"
#include "monobucket/version.hpp"
#include "server/asset_store.hpp"
#include "server/server.hpp"

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

std::string renderMetrics(const Config& config) {
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

    os << "# HELP monobucket_cache_limit_bytes Configured cache budget.\n"
       << "# TYPE monobucket_cache_limit_bytes gauge\n"
       << "monobucket_cache_limit_bytes " << config.cacheMaxBytes << '\n';

    os << "# HELP monobucket_embedded_asset_bytes Size of the dashboard baked into the binary.\n"
       << "# TYPE monobucket_embedded_asset_bytes gauge\n"
       << "monobucket_embedded_asset_bytes " << assets::totalBytes() << '\n';

    // Phase 3/4 append cache hit-ratio, request and object counters here.
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

void registerSystemRoutes(const Config& config) {
    auto& app = drogon::app();

    // Liveness: answers as long as the event loop is turning.
    app.registerHandler(
        "/healthz",
        [](const HttpRequestPtr&, ResponseCallback&& callback) {
            callback(jsonResponse({{"status", "ok"}, {"version", version::kVersion}}));
        },
        {drogon::Get, drogon::Head});

    // Readiness: Phase 2 extends this with a metadata-store probe.
    app.registerHandler(
        "/readyz",
        [&config](const HttpRequestPtr&, ResponseCallback&& callback) {
            callback(jsonResponse({
                {"status", "ready"},
                {"dataDir", config.dataDir},
                {"workerThreads", config.workerThreads},
            }));
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
            [&config](const HttpRequestPtr&, ResponseCallback&& callback) {
                callback(textResponse(renderMetrics(config), "text/plain; version=0.0.4"));
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

void registerConsoleRoutes(const Config& config) {
    if (!assets::embedded()) {
        // Built without the dashboard: leave the routes unregistered so the
        // console port answers 404 instead of a blank page.
        return;
    }

    drogon::app().registerHandlerViaRegex(
        "^/.*$",
        [&config](const HttpRequestPtr& req, ResponseCallback&& callback) {
            if (!onConsoleListener(req, config)) {
                auto resp = HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k404NotFound);
                callback(resp);
                return;
            }

            const std::string path = req->path();
            const assets::Asset* asset = assets::find(path);
            if (asset == nullptr && (path == "/" || path.find('.') == std::string::npos)) {
                // Client-side routing: unknown extension-less paths get the SPA
                // shell and let SvelteKit resolve the route.
                asset = assets::indexDocument();
            }

            if (asset == nullptr) {
                auto resp = HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k404NotFound);
                callback(resp);
                return;
            }

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
            callback(resp);
        },
        {drogon::Get, drogon::Head});
}

}  // namespace monobucket
