#include <algorithm>
#include <charconv>
#include <limits>

#include "core/config.hpp"
#include "s3/handlers.hpp"
#include "s3/response.hpp"
#include "s3/s3_error.hpp"
#include "s3/xml.hpp"
#include "storage/storage_engine.hpp"

namespace monobucket::s3 {
namespace {

constexpr const char* kStorageClass = "STANDARD";

std::uint32_t parseUnsigned(std::string_view raw, std::string_view name,
                            std::uint32_t maximum) {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (raw.empty() || error != std::errc() || end != raw.data() + raw.size() ||
        value > maximum) {
        throw S3Exception(S3ErrorCode::InvalidArgument,
                          std::string(name) + " must be a non-negative number.");
    }
    return static_cast<std::uint32_t>(value);
}

std::uint32_t parsePartNumber(const S3Request& request) {
    const auto raw = request.queryValue("partNumber");
    if (!raw || raw->empty()) {
        throw S3Exception(S3ErrorCode::InvalidArgument, "partNumber is required.");
    }

    const std::uint32_t value = parseUnsigned(*raw, "partNumber", limits::kMaxPartNumber);
    if (value == 0) {
        throw S3Exception(S3ErrorCode::InvalidArgument,
                          "Part number must be between 1 and " +
                              std::to_string(limits::kMaxPartNumber) + ".");
    }
    return value;
}

std::string requireUploadId(const S3Request& request) {
    const auto uploadId = request.queryValue("uploadId");
    if (!uploadId || uploadId->empty()) throw S3Exception(S3ErrorCode::NoSuchUpload);
    return *uploadId;
}

/// Confirms the upload belongs to the bucket and key in the request path. The
/// upload id alone is enough to find it, so without this check any upload could
/// be completed into any key the caller named.
UploadRecord requireUpload(const S3Context& context, const S3Request& request,
                           std::string_view uploadId) {
    const auto upload = context.storage.getUpload(uploadId);
    if (!upload) throw S3Exception(S3ErrorCode::NoSuchUpload);

    if (upload->bucket != request.bucket || upload->key != request.key) {
        throw S3Exception(S3ErrorCode::NoSuchUpload,
                          "The upload id does not belong to the bucket and key named in the "
                          "request.");
    }
    return *upload;
}

}  // namespace

drogon::HttpResponsePtr handleCreateMultipartUpload(const S3Context& context,
                                                    const S3Request& request,
                                                    const drogon::HttpRequestPtr& http) {
    requireBucket(context, request.bucket);
    if (!isValidObjectKey(request.key)) {
        throw S3Exception(S3ErrorCode::InvalidArgument, "The object key is not valid.");
    }

    StorageEngine::PutRequest put;
    put.bucket       = request.bucket;
    put.key          = request.key;
    put.contentType  = contentTypeOf(http);
    put.userMetadata = collectUserMetadata(http);

    const std::string uploadId = context.storage.createUpload(put);

    XmlWriter writer("InitiateMultipartUploadResult");
    writer.element("Bucket", request.bucket);
    writer.element("Key", request.key);
    writer.element("UploadId", uploadId);

    auto response = xmlResponse(writer.finish());
    applyCommonHeaders(response, request.requestId);
    return response;
}

drogon::HttpResponsePtr handleUploadPart(const S3Context& context, const S3Request& request,
                                         const S3Body& body) {
    const std::string  uploadId   = requireUploadId(request);
    const std::uint32_t partNumber = parsePartNumber(request);
    requireUpload(context, request, uploadId);

    BlobWriter    writer  = context.storage.beginWrite();
    std::uint64_t written = 0;

    body.streamTo([&writer, &written](std::string_view chunk) {
        writer.write(chunk);
        written += chunk.size();
    });

    const PartRecord part = context.storage.finishPart(uploadId, partNumber, std::move(writer));
    context.metrics.bytesIn.fetch_add(written, std::memory_order_relaxed);

    auto response = emptyResponse(200);
    response->addHeader(headers::kETag, quoteETag(part.etag));
    applyCommonHeaders(response, request.requestId);
    return response;
}

drogon::HttpResponsePtr handleListParts(const S3Context& context, const S3Request& request) {
    const std::string  uploadId = requireUploadId(request);
    const UploadRecord upload   = requireUpload(context, request, uploadId);

    // The marker is exclusive, and defaults to zero — part numbers start at one.
    std::uint32_t marker = 0;
    if (const auto raw = request.queryValue("part-number-marker"); raw && !raw->empty()) {
        marker = parseUnsigned(*raw, "part-number-marker",
                               std::numeric_limits<std::uint32_t>::max());
    }

    std::uint32_t maxParts = 1000;
    if (const auto raw = request.queryValue("max-parts"); raw && !raw->empty()) {
        maxParts = std::min<std::uint32_t>(parseUnsigned(
                                              *raw, "max-parts",
                                              std::numeric_limits<std::uint32_t>::max()),
                                          1000);
    }

    auto parts = context.storage.listParts(uploadId);
    parts.erase(std::remove_if(parts.begin(), parts.end(),
                               [marker](const PartRecord& part) {
                                   return part.partNumber <= marker;
                               }),
                parts.end());

    const bool truncated = parts.size() > maxParts;
    if (truncated) parts.resize(maxParts);

    XmlWriter writer("ListPartsResult");
    writer.element("Bucket", upload.bucket);
    writer.element("Key", upload.key);
    writer.element("UploadId", uploadId);
    writer.element("PartNumberMarker", static_cast<std::uint64_t>(marker));
    if (truncated && !parts.empty()) {
        writer.element("NextPartNumberMarker", static_cast<std::uint64_t>(parts.back().partNumber));
    }
    writer.element("MaxParts", static_cast<std::uint64_t>(maxParts));
    writer.booleanElement("IsTruncated", truncated);
    writer.element("StorageClass", kStorageClass);

    for (const auto& part : parts) {
        writer.open("Part");
        writer.element("PartNumber", static_cast<std::uint64_t>(part.partNumber));
        writer.element("LastModified", toIso8601(part.uploadedAt));
        writer.element("ETag", quoteETag(part.etag));
        writer.element("Size", part.size);
        writer.close();
    }

    auto response = xmlResponse(writer.finish());
    applyCommonHeaders(response, request.requestId);
    return response;
}

drogon::HttpResponsePtr handleCompleteMultipartUpload(const S3Context& context,
                                                      const S3Request& request,
                                                      const S3Body& body) {
    const std::string uploadId = requireUploadId(request);
    requireUpload(context, request, uploadId);

    XmlNode root;
    try {
        root = parseXml(body.materialise());
    } catch (const XmlParseError& error) {
        throw S3Exception(S3ErrorCode::MalformedXML, error.what());
    }
    if (root.name != "CompleteMultipartUpload") {
        throw S3Exception(S3ErrorCode::MalformedXML,
                          "The root element must be <CompleteMultipartUpload>.");
    }

    std::vector<StorageEngine::RequestedPart> manifest;
    std::uint32_t                             previous = 0;

    for (const XmlNode* entry : root.childrenNamed("Part")) {
        StorageEngine::RequestedPart part;

        const std::string number = entry->childText("PartNumber");
        std::uint64_t parsed = 0;
        const auto [end, error] =
            std::from_chars(number.data(), number.data() + number.size(), parsed);
        if (number.empty() || error != std::errc() || end != number.data() + number.size() ||
            parsed < 1 || parsed > limits::kMaxPartNumber) {
            throw S3Exception(S3ErrorCode::MalformedXML,
                              "PartNumber '" + number + "' is not valid.");
        }
        part.partNumber = static_cast<std::uint32_t>(parsed);

        // Checked here rather than left to the storage layer so the client gets
        // the code S3 defines for it; every other manifest problem is a part
        // that does not match, which is InvalidPart either way.
        if (part.partNumber <= previous) {
            throw S3Exception(S3ErrorCode::InvalidPartOrder);
        }
        previous = part.partNumber;

        part.etag = entry->childText("ETag");
        if (part.etag.empty()) {
            throw S3Exception(S3ErrorCode::MalformedXML,
                              "Part " + number + " carries no ETag.");
        }
        manifest.push_back(std::move(part));
    }

    if (manifest.empty()) {
        throw S3Exception(S3ErrorCode::MalformedXML, "The manifest lists no parts.");
    }

    const ObjectRecord record = context.storage.completeUpload(uploadId, manifest);
    invalidateObject(context, request.bucket, request.key);

    XmlWriter writer("CompleteMultipartUploadResult");
    // The Location is echoed by clients; a path-style URL is the only form we
    // can build without knowing how the client reached us.
    writer.element("Location", "/" + request.bucket + "/" + uriEncode(request.key, false));
    writer.element("Bucket", request.bucket);
    writer.element("Key", request.key);
    writer.element("ETag", quoteETag(record.etag));

    auto response = xmlResponse(writer.finish());
    applyCommonHeaders(response, request.requestId);
    return response;
}

drogon::HttpResponsePtr handleAbortMultipartUpload(const S3Context& context,
                                                   const S3Request& request) {
    const std::string uploadId = requireUploadId(request);
    requireUpload(context, request, uploadId);

    context.storage.abortUpload(uploadId);

    auto response = emptyResponse(204);
    applyCommonHeaders(response, request.requestId);
    return response;
}

drogon::HttpResponsePtr handleListMultipartUploads(const S3Context& context,
                                                   const S3Request& request) {
    requireBucket(context, request.bucket);

    std::uint32_t maxUploads = 1000;
    if (const auto raw = request.queryValue("max-uploads"); raw && !raw->empty()) {
        maxUploads = std::min<std::uint32_t>(parseUnsigned(
                                                *raw, "max-uploads",
                                                std::numeric_limits<std::uint32_t>::max()),
                                            1000);
    }

    const std::string prefix   = request.queryOr("prefix");
    auto              uploads  = context.storage.listUploads(request.bucket, maxUploads);

    if (!prefix.empty()) {
        uploads.erase(std::remove_if(uploads.begin(), uploads.end(),
                                     [&prefix](const UploadRecord& upload) {
                                         return upload.key.rfind(prefix, 0) != 0;
                                     }),
                      uploads.end());
    }

    XmlWriter writer("ListMultipartUploadsResult");
    writer.element("Bucket", request.bucket);
    writer.element("KeyMarker", "");
    writer.element("UploadIdMarker", "");
    writer.element("MaxUploads", static_cast<std::uint64_t>(maxUploads));
    writer.elementIfSet("Prefix", prefix);
    writer.elementIfSet("Delimiter", request.queryOr("delimiter"));
    // Pagination is bounded by max-uploads at the storage layer, so a listing
    // is never truncated in a way the client could resume — the marker fields
    // above are reported empty rather than invented.
    writer.booleanElement("IsTruncated", false);

    for (const auto& upload : uploads) {
        writer.open("Upload");
        writer.element("Key", upload.key);
        writer.element("UploadId", upload.uploadId);
        writer.element("Initiated", toIso8601(upload.createdAt));
        writer.element("StorageClass", kStorageClass);
        writer.close();
    }

    auto response = xmlResponse(writer.finish());
    applyCommonHeaders(response, request.requestId);
    return response;
}

}  // namespace monobucket::s3
