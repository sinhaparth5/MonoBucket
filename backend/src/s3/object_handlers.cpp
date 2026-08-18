#include <algorithm>

#include "core/config.hpp"
#include "s3/handlers.hpp"
#include "s3/response.hpp"
#include "s3/s3_error.hpp"
#include "s3/xml.hpp"
#include "storage/storage_engine.hpp"

namespace monobucket::s3 {
namespace {

/// S3 caps one DeleteObjects request at a thousand keys.
constexpr std::size_t kMaxDeleteKeys = 1000;

/// Query parameters that override response headers. Used by presigned links
/// that want a browser to download rather than render, which is precisely what
/// the dashboard's link generator needs.
struct ResponseOverride {
    const char* parameter;
    const char* header;
};

constexpr ResponseOverride kOverrides[] = {
    {"response-content-type",        "Content-Type"},
    {"response-content-disposition", "Content-Disposition"},
    {"response-content-encoding",    "Content-Encoding"},
    {"response-content-language",    "Content-Language"},
    {"response-cache-control",       "Cache-Control"},
    {"response-expires",             "Expires"},
};

bool etagMatches(std::string_view header, std::string_view etag) {
    // `If-Match: *` means "any current version", which for a key that exists is
    // always satisfied.
    if (header == "*") return true;

    std::size_t pos = 0;
    while (pos <= header.size()) {
        const std::size_t comma = header.find(',', pos);
        const std::size_t end   = (comma == std::string_view::npos) ? header.size() : comma;

        std::string_view candidate = header.substr(pos, end - pos);
        while (!candidate.empty() && candidate.front() == ' ') candidate.remove_prefix(1);
        while (!candidate.empty() && candidate.back() == ' ') candidate.remove_suffix(1);

        if (unquoteETag(candidate) == etag) return true;
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return false;
}

/// RFC 7232 precondition evaluation, in the order S3 applies it. Returns 0 to
/// proceed, or the status the request should be answered with.
int evaluatePreconditions(const drogon::HttpRequestPtr& http, const ObjectRecord& record) {
    const std::string& ifMatch          = http->getHeader("if-match");
    const std::string& ifNoneMatch      = http->getHeader("if-none-match");
    const std::string& ifModified       = http->getHeader("if-modified-since");
    const std::string& ifUnmodified     = http->getHeader("if-unmodified-since");

    if (!ifMatch.empty()) {
        if (!etagMatches(ifMatch, record.etag)) return 412;
    } else if (!ifUnmodified.empty()) {
        // Only consulted when If-Match is absent: an entity tag is a stronger
        // statement than a timestamp, and a second's resolution cannot express
        // two writes in the same second.
        const auto since = parseHttpDate(ifUnmodified);
        if (since && record.lastModified / 1000 > *since / 1000) return 412;
    }

    if (!ifNoneMatch.empty()) {
        if (etagMatches(ifNoneMatch, record.etag)) return 304;
    } else if (!ifModified.empty()) {
        const auto since = parseHttpDate(ifModified);
        if (since && record.lastModified / 1000 <= *since / 1000) return 304;
    }

    return 0;
}

void applyObjectHeaders(const drogon::HttpResponsePtr& response, const ObjectRecord& record) {
    response->addHeader(headers::kETag, quoteETag(record.etag));
    response->addHeader("Last-Modified", toHttpDate(record.lastModified));
    response->addHeader("Accept-Ranges", "bytes");

    for (const auto& [name, value] : record.userMetadata) {
        response->addHeader("x-amz-meta-" + name, value);
    }
}

void applyOverrides(const drogon::HttpResponsePtr& response, const S3Request& request) {
    for (const auto& [parameter, header] : kOverrides) {
        const auto value = request.queryValue(parameter);
        if (!value || value->empty()) continue;

        if (std::string_view(header) == "Content-Type") {
            response->setContentTypeString(*value);
        } else {
            response->addHeader(header, *value);
        }
    }
}

void requireValidKey(std::string_view key) {
    if (key.size() > limits::kMaxKeyLength) throw S3Exception(S3ErrorCode::KeyTooLong);
    if (!isValidObjectKey(key)) {
        throw S3Exception(S3ErrorCode::InvalidArgument,
                          "The object key is empty, or contains characters that cannot be "
                          "represented in a listing.");
    }
}

}  // namespace

drogon::HttpResponsePtr handleGetObject(const S3Context& context, const S3Request& request,
                                        const drogon::HttpRequestPtr& http) {
    requireBucket(context, request.bucket);

    const auto record = statObject(context, request.bucket, request.key);
    if (!record) throw S3Exception(S3ErrorCode::NoSuchKey);

    if (const int precondition = evaluatePreconditions(http, *record); precondition != 0) {
        // 304 must not carry a body (RFC 9110), but it does carry the
        // validators, so the client can refresh its own copy's metadata
        // without a second round trip. 412 is an ordinary error and S3 sends
        // the error document with it — a client that branches on the code
        // string has nothing to read otherwise.
        if (precondition == 412) {
            return errorResponse(S3ErrorCode::PreconditionFailed, "", request.resource,
                                 request.requestId);
        }

        auto response = emptyResponse(precondition);
        response->addHeader(headers::kETag, quoteETag(record->etag));
        response->addHeader("Last-Modified", toHttpDate(record->lastModified));
        applyCommonHeaders(response, request.requestId);
        return response;
    }

    ByteRange         range;
    const RangeResult ranged = parseRange(http->getHeader("range"), record->size, range);

    if (ranged == RangeResult::Unsatisfiable) {
        auto response = errorResponse(S3ErrorCode::InvalidRange, "", request.resource,
                                      request.requestId);
        response->addHeader("Content-Range", "bytes */" + std::to_string(record->size));
        return response;
    }

    const std::string path = context.storage.blobs().pathFor(record->blobId).string();

    // The payload is streamed by the framework — sendfile for anything large
    // enough to matter — so a multi-gigabyte GET never materialises in our
    // address space.
    auto response = drogon::HttpResponse::newFileResponse(
        path, ranged == RangeResult::Satisfiable ? range.offset : 0,
        ranged == RangeResult::Satisfiable ? range.length : 0,
        ranged == RangeResult::Satisfiable, "", drogon::CT_NONE, record->contentType);

    if (response->getStatusCode() == drogon::k404NotFound) {
        // Metadata names a payload the tree does not have. That is corruption,
        // not a missing key, and saying so is what makes it get investigated.
        throw S3Exception(S3ErrorCode::InternalError,
                          "The payload for this object is missing from the store.");
    }

    if (ranged == RangeResult::Satisfiable) {
        // Drogon answers 200 when the range happens to cover the whole object.
        // A client that asked for a range is entitled to 206 either way, and at
        // least one SDK treats a 200 to a ranged GET as a server that ignored
        // the header and buffers accordingly.
        response->setStatusCode(drogon::k206PartialContent);
    }

    applyObjectHeaders(response, *record);
    applyOverrides(response, request);
    applyCommonHeaders(response, request.requestId);

    context.metrics.bytesOut.fetch_add(
        ranged == RangeResult::Satisfiable ? range.length : record->size,
        std::memory_order_relaxed);

    return response;
}

drogon::HttpResponsePtr handlePutObject(const S3Context& context, const S3Request& request,
                                        const drogon::HttpRequestPtr& http, const S3Body& body) {
    requireBucket(context, request.bucket);
    requireValidKey(request.key);

    // A copy is a different operation with a different body contract; silently
    // storing the header's value as an object would be worse than refusing.
    if (!http->getHeader("x-amz-copy-source").empty()) {
        throw S3Exception(S3ErrorCode::NotImplemented,
                          "CopyObject is not implemented; upload the object directly.");
    }

    StorageEngine::PutRequest put;
    put.bucket       = request.bucket;
    put.key          = request.key;
    put.contentType  = contentTypeOf(http);
    put.userMetadata = collectUserMetadata(http);

    // Claimed before a byte is written to the tree, so a bucket that is full
    // refuses the request instead of storing a payload it will then have to
    // reclaim. finishWrite settles the claim against what actually arrived.
    auto reservation = context.storage.reserveSpace(request.bucket, body.decodedLength());

    BlobWriter writer = context.storage.beginWrite();
    std::uint64_t written = 0;

    // Verification happens inside streamTo, before any byte reaches the writer,
    // and an exception here leaves the writer to discard its own temporary.
    body.streamTo([&writer, &written](std::string_view chunk) {
        writer.write(chunk);
        written += chunk.size();
    });

    const ObjectRecord record =
        context.storage.finishWrite(put, std::move(writer), std::move(reservation));

    // After the write, not before: a failed PUT must not invalidate a cached
    // entry that is still the current one.
    invalidateObject(context, request.bucket, request.key);

    context.metrics.bytesIn.fetch_add(written, std::memory_order_relaxed);

    auto response = emptyResponse(200);
    response->addHeader(headers::kETag, quoteETag(record.etag));
    applyCommonHeaders(response, request.requestId);
    return response;
}

drogon::HttpResponsePtr handleDeleteObject(const S3Context& context, const S3Request& request) {
    requireBucket(context, request.bucket);

    context.storage.deleteObject(request.bucket, request.key);
    invalidateObject(context, request.bucket, request.key);

    // 204 whether or not the key existed. S3 defines DELETE as idempotent, and
    // a client retrying after a dropped response must not see a different
    // answer than it would have got the first time.
    auto response = emptyResponse(204);
    applyCommonHeaders(response, request.requestId);
    return response;
}

drogon::HttpResponsePtr handleDeleteObjects(const S3Context& context, const S3Request& request,
                                            const drogon::HttpRequestPtr& http,
                                            const S3Body& body) {
    requireBucket(context, request.bucket);

    const std::string document = body.materialise();
    verifyContentMd5(http, document);

    XmlNode root;
    try {
        root = parseXml(document);
    } catch (const XmlParseError& error) {
        throw S3Exception(S3ErrorCode::MalformedXML, error.what());
    }
    if (root.name != "Delete") {
        throw S3Exception(S3ErrorCode::MalformedXML, "The root element must be <Delete>.");
    }

    const bool quiet = root.childText("Quiet") == "true";

    const auto entries = root.childrenNamed("Object");
    if (entries.empty()) {
        throw S3Exception(S3ErrorCode::MalformedXML, "The request listed no objects to delete.");
    }
    if (entries.size() > kMaxDeleteKeys) {
        throw S3Exception(S3ErrorCode::MalformedXML,
                          "A single request may delete at most " + std::to_string(kMaxDeleteKeys) +
                              " objects.");
    }

    XmlWriter writer("DeleteResult");

    for (const XmlNode* entry : entries) {
        const std::string key = entry->childText("Key");

        // Each key is reported individually. One bad key must not fail the
        // batch — a client deleting a thousand objects has no way to tell which
        // one was the problem from a single error document.
        try {
            if (key.empty()) throw S3Exception(S3ErrorCode::InvalidArgument, "The key is empty.");

            context.storage.deleteObject(request.bucket, key);
            invalidateObject(context, request.bucket, key);

            if (!quiet) {
                writer.open("Deleted");
                writer.element("Key", key);
                writer.close();
            }
        } catch (const S3Exception& error) {
            writer.open("Error");
            writer.element("Key", key);
            writer.element("Code", describe(error.code()).code);
            writer.element("Message", error.what());
            writer.close();
        } catch (const StorageError& error) {
            writer.open("Error");
            writer.element("Key", key);
            writer.element("Code", describe(fromStorage(error.code())).code);
            writer.element("Message", error.what());
            writer.close();
        }
    }

    auto response = xmlResponse(writer.finish());
    applyCommonHeaders(response, request.requestId);
    return response;
}

}  // namespace monobucket::s3
