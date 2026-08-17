#include "s3/router.hpp"

#include <chrono>
#include <memory>
#include <sstream>

#include <drogon/drogon.h>

#include "cache/cache_provider.hpp"
#include "core/config.hpp"
#include "core/io_executor.hpp"
#include "core/logging.hpp"
#include "s3/cors.hpp"
#include "s3/handlers.hpp"
#include "s3/operation.hpp"
#include "s3/request.hpp"
#include "s3/response.hpp"
#include "s3/s3_error.hpp"
#include "s3/sigv4.hpp"
#include "server/system_routes.hpp"
#include "storage/storage_engine.hpp"

namespace monobucket::s3 {
namespace {

using drogon::HttpRequestPtr;
using drogon::HttpResponsePtr;
using ResponseCallback = std::function<void(const HttpResponsePtr&)>;

/// Everything the handler lambda needs, held by shared_ptr so the closure owns
/// a stable address rather than capturing five references into the Server.
struct Router {
    S3Context   context;
    IoExecutor& io;
};

std::int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::uint64_t headerAsUnsigned(const HttpRequestPtr& request, const char* name) {
    const std::string& value = request->getHeader(name);
    if (value.empty()) return 0;
    try {
        return std::stoull(value);
    } catch (const std::exception&) {
        return 0;
    }
}

SigningRequest toSigningRequest(const HttpRequestPtr& request) {
    SigningRequest signing;
    // methodString() reflects what Drogon routed, which is GET for a HEAD. The
    // canonical request needs the method the client signed, so verification
    // tries the alternate — see AuthOptions::alternateMethod.
    signing.method = request->methodString();
    signing.uri    = request->getOriginalPath();
    signing.query  = request->query();

    signing.headers.reserve(request->getHeaders().size());
    for (const auto& [name, value] : request->getHeaders()) {
        signing.headers.emplace_back(name, value);
    }
    return signing;
}

/// Anonymous access is allowed only where a bucket policy or ACL says so, and
/// only for operations that cannot change anything.
void authorize(const S3Context& context, const S3Request& request, Operation operation,
               const AuthOutcome& auth) {
    if (!auth.anonymous) return;

    if (!isReadOnly(operation)) throw S3Exception(S3ErrorCode::AccessDenied);

    // Listing every bucket in the server is never public: it would enumerate
    // the private buckets alongside the public one.
    if (request.bucket.empty()) throw S3Exception(S3ErrorCode::AccessDenied);

    const BucketRecord bucket = requireBucket(context, request.bucket);
    if (!bucket.publicRead) throw S3Exception(S3ErrorCode::AccessDenied);

    context.metrics.anonymous.fetch_add(1, std::memory_order_relaxed);
}

HttpResponsePtr dispatch(const S3Context& context, const S3Request& request, Operation operation,
                         const HttpRequestPtr& http, const S3Body& body) {
    switch (operation) {
        case Operation::ListBuckets:    return handleListBuckets(context, request);
        case Operation::CreateBucket:   return handleCreateBucket(context, request);
        case Operation::DeleteBucket:   return handleDeleteBucket(context, request);
        case Operation::HeadBucket:     return handleHeadBucket(context, request);
        case Operation::ListObjectsV1:  return handleListObjects(context, request, false);
        case Operation::ListObjectsV2:  return handleListObjects(context, request, true);
        case Operation::GetBucketLocation:   return handleGetBucketLocation(context, request);
        case Operation::GetBucketVersioning: return handleGetBucketVersioning(context, request);
        case Operation::GetBucketAcl:        return handleGetBucketAcl(context, request);
        case Operation::PutBucketAcl:        return handlePutBucketAcl(context, request, http, body);
        case Operation::GetBucketPolicy:     return handleGetBucketPolicy(context, request);
        case Operation::PutBucketPolicy:     return handlePutBucketPolicy(context, request, body);
        case Operation::DeleteBucketPolicy:  return handleDeleteBucketPolicy(context, request);
        case Operation::GetBucketCors:       return handleGetBucketCors(context, request);
        case Operation::PutBucketCors:       return handlePutBucketCors(context, request, body);
        case Operation::DeleteBucketCors:    return handleDeleteBucketCors(context, request);

        // Normally answered by serve() before authentication runs. Reaching
        // here would mean that branch was removed, so it stays correct rather
        // than falling through to "not implemented".
        case Operation::Preflight:      return handlePreflight(context, request, http);
        case Operation::DeleteObjects:  return handleDeleteObjects(context, request, http, body);

        // HeadObject arrives here too — Drogon rewrites HEAD to GET before
        // routing and drops the body on the way out, so one handler serves both.
        case Operation::GetObject:
        case Operation::HeadObject:     return handleGetObject(context, request, http);
        case Operation::PutObject:      return handlePutObject(context, request, http, body);
        case Operation::DeleteObject:   return handleDeleteObject(context, request);

        case Operation::CreateMultipartUpload:
            return handleCreateMultipartUpload(context, request, http);
        case Operation::UploadPart:     return handleUploadPart(context, request, body);
        case Operation::ListParts:      return handleListParts(context, request);
        case Operation::CompleteMultipartUpload:
            return handleCompleteMultipartUpload(context, request, body);
        case Operation::AbortMultipartUpload:
            return handleAbortMultipartUpload(context, request);
        case Operation::ListMultipartUploads:
            return handleListMultipartUploads(context, request);

        case Operation::Unsupported:
            throw S3Exception(S3ErrorCode::NotImplemented);
        case Operation::MethodNotAllowed:
            throw S3Exception(S3ErrorCode::MethodNotAllowed);
    }
    throw S3Exception(S3ErrorCode::NotImplemented);
}

/// Runs the whole request on an I/O thread and turns every failure into an S3
/// error document. Nothing above this catches — a handler that throws is
/// reporting a condition, not losing control.
HttpResponsePtr serve(const S3Context& context, const HttpRequestPtr& http) {
    S3Request request = parseRequest(http->methodString(), http->path(), http->query(),
                                     virtualHostBucket(http->getHeader("host"),
                                                       context.config.s3Domain));

    HttpResponsePtr response;

    try {
        // A preflight is answered before anything looks for a signature. A
        // browser never attaches credentials to one, so verifying here would
        // refuse every cross-origin request to a private bucket before the
        // real, signed request was ever sent. Arrives via the pre-routing
        // advice rather than the route — see registerS3Routes.
        if (request.method == "OPTIONS") {
            return handlePreflight(context, request, http);
        }

        AuthOptions options;
        options.region     = context.config.region;
        options.nowSeconds = nowSeconds();

        // Drogon has already rewritten a HEAD into a GET, so the signature may
        // have been computed over either. Whichever verifies is the method the
        // client sent — which is also how the original method is recovered at
        // all, since the framework keeps no record of it.
        if (request.method == "GET") options.alternateMethod = "HEAD";

        const Credentials credentials{context.config.rootAccessKey, context.config.rootSecretKey};
        const AuthOutcome auth =
            authenticate(toSigningRequest(http), request.query, credentials, options);

        if (!auth.method.empty()) request.method = auth.method;

        std::string     unsupported;
        const Operation operation = classify(request, unsupported);
        if (operation == Operation::Unsupported && !unsupported.empty()) {
            throw S3Exception(S3ErrorCode::NotImplemented,
                              "The '" + unsupported + "' subresource is not implemented.");
        }

        authorize(context, request, operation, auth);

        const S3Body body(http->getBody(), auth, http->getHeader("content-encoding"),
                          headerAsUnsigned(http, "x-amz-decoded-content-length"));

        response = dispatch(context, request, operation, http, body);
    } catch (const S3Exception& error) {
        const S3ErrorInfo& info = describe(error.code());
        if (info.status == 403) {
            context.metrics.authFailures.fetch_add(1, std::memory_order_relaxed);
        }
        response = errorResponse(error.code(), error.what(), request.resource, request.requestId);
    } catch (const StorageError& error) {
        const S3ErrorCode code = fromStorage(error.code());
        if (describe(code).status >= 500) {
            log::error("storage failure serving ", request.method, ' ', request.resource, ": ",
                       error.what());
        }
        response = errorResponse(code, error.what(), request.resource, request.requestId);
    } catch (const std::exception& error) {
        // The message goes to the log, not to the client: an unexpected
        // exception's text is as likely to name a path on our disk as anything
        // the caller could act on.
        log::error("unhandled failure serving ", request.method, ' ', request.resource, " [",
                   request.requestId, "]: ", error.what());
        response =
            errorResponse(S3ErrorCode::InternalError, "", request.resource, request.requestId);
    }

    // Errors are decorated too. A script that cannot read the 403 it got back
    // reports a network failure instead, which sends whoever is debugging it
    // looking at the wrong layer.
    applyCorsHeaders(context, request, http, response);
    return response;
}

/// Runs `serve` on an I/O thread and answers, or sheds when the queue is full.
/// Every S3 operation touches RocksDB or the filesystem, so none of them may
/// run on an event loop thread. Posting the whole request — authentication
/// included, since verifying a body digest is not cheap — keeps the loop free
/// to accept connections while the disk is busy.
void offload(const std::shared_ptr<Router>& router, const HttpRequestPtr& http,
             const ResponseCallback& callback) {
    router->context.metrics.requests.fetch_add(1, std::memory_order_relaxed);

    const bool accepted = router->io.post([router, http, callback]() mutable {
        auto response = serve(router->context, http);
        router->context.metrics.observe(static_cast<int>(response->getStatusCode()));
        callback(response);
    });
    if (accepted) return;

    // Shedding load rather than queueing it is the whole point of a bounded
    // queue. SlowDown is the code the AWS CLI backs off on.
    router->context.metrics.shed.fetch_add(1, std::memory_order_relaxed);
    router->context.metrics.observe(503);

    auto response = errorResponse(S3ErrorCode::SlowDown, "", http->path(), newRequestId());
    response->addHeader("Retry-After", "1");
    callback(response);
}

}  // namespace

void registerS3Routes(const Config& config, StorageEngine& storage, IoExecutor& io,
                      CacheProvider& cache, S3Metrics& metrics) {
    auto router = std::make_shared<Router>(Router{S3Context{config, storage, cache, metrics}, io});

    // Drogon answers OPTIONS itself, before routing and before any handler
    // runs, with `Access-Control-Allow-Origin: *` and every method the route
    // was registered for. That is the one answer a preflight must never get:
    // it tells any origin that any method is permitted, whatever the bucket's
    // rules say. There is no hook inside that path, so the request is
    // intercepted ahead of it — pre-routing advice is the earliest point at
    // which a response can be substituted. The route below therefore does not
    // register Options at all; nothing would ever reach it.
    drogon::app().registerPreRoutingAdvice(
        [router](const HttpRequestPtr& http, drogon::AdviceCallback&& respond,
                 drogon::AdviceChainCallback&& next) {
            const Config& settings = router->context.config;
            const bool    isConsole =
                settings.consoleEnabled && http->getLocalAddr().toPort() == settings.consolePort;

            if (http->method() != drogon::Options || isConsole) {
                next();
                return;
            }
            offload(router, http, respond);
        });

    drogon::app().registerHandlerViaRegex(
        "^/.*$",
        [router](const HttpRequestPtr& http, ResponseCallback&& callback) {
            const Config& settings = router->context.config;

            if (settings.consoleEnabled &&
                http->getLocalAddr().toPort() == settings.consolePort) {
                auto response = consoleAssetResponse(http->path(), http->getHeader("accept-encoding"));
                if (!response) {
                    response = drogon::HttpResponse::newHttpResponse();
                    response->setStatusCode(drogon::k404NotFound);
                }
                callback(response);
                return;
            }

            offload(router, http, callback);
        },
        {drogon::Get, drogon::Post, drogon::Put, drogon::Delete, drogon::Head});
}

std::string renderS3Metrics(const S3Metrics& metrics) {
    std::ostringstream os;

    os << "# HELP monobucket_s3_requests_total Requests accepted on the S3 listener.\n"
       << "# TYPE monobucket_s3_requests_total counter\n"
       << "monobucket_s3_requests_total "
       << metrics.requests.load(std::memory_order_relaxed) << '\n';

    os << "# HELP monobucket_s3_responses_total Responses by status class.\n"
       << "# TYPE monobucket_s3_responses_total counter\n"
       << "monobucket_s3_responses_total{class=\"2xx\"} "
       << metrics.succeeded.load(std::memory_order_relaxed) << '\n'
       << "monobucket_s3_responses_total{class=\"4xx\"} "
       << metrics.clientErrors.load(std::memory_order_relaxed) << '\n'
       << "monobucket_s3_responses_total{class=\"5xx\"} "
       << metrics.serverErrors.load(std::memory_order_relaxed) << '\n';

    // Distinct from the 5xx count on purpose: shedding load is the bounded
    // queue working, not the server failing, and the two deserve different
    // alerting rules.
    os << "# HELP monobucket_s3_shed_total Requests refused because the storage queue was full.\n"
       << "# TYPE monobucket_s3_shed_total counter\n"
       << "monobucket_s3_shed_total " << metrics.shed.load(std::memory_order_relaxed) << '\n';

    os << "# HELP monobucket_s3_auth_failures_total Requests rejected by signature verification "
          "or authorisation.\n"
       << "# TYPE monobucket_s3_auth_failures_total counter\n"
       << "monobucket_s3_auth_failures_total "
       << metrics.authFailures.load(std::memory_order_relaxed) << '\n';

    os << "# HELP monobucket_s3_anonymous_total Requests served without credentials against a "
          "public bucket.\n"
       << "# TYPE monobucket_s3_anonymous_total counter\n"
       << "monobucket_s3_anonymous_total " << metrics.anonymous.load(std::memory_order_relaxed)
       << '\n';

    // Payload bytes only, so these are comparable with monobucket_object_bytes
    // rather than with a network counter.
    os << "# HELP monobucket_s3_payload_bytes_in_total Object payload bytes received.\n"
       << "# TYPE monobucket_s3_payload_bytes_in_total counter\n"
       << "monobucket_s3_payload_bytes_in_total " << metrics.bytesIn.load(std::memory_order_relaxed)
       << '\n';

    os << "# HELP monobucket_s3_payload_bytes_out_total Object payload bytes sent.\n"
       << "# TYPE monobucket_s3_payload_bytes_out_total counter\n"
       << "monobucket_s3_payload_bytes_out_total "
       << metrics.bytesOut.load(std::memory_order_relaxed) << '\n';

    return os.str();
}

}  // namespace monobucket::s3
