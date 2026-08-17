#include <algorithm>
#include <charconv>

#include <nlohmann/json.hpp>

#include "core/config.hpp"
#include "s3/base64.hpp"
#include "s3/handlers.hpp"
#include "s3/response.hpp"
#include "s3/s3_error.hpp"
#include "s3/xml.hpp"
#include "storage/digest.hpp"
#include "storage/storage_engine.hpp"

namespace monobucket::s3 {
namespace {

constexpr const char* kStorageClass = "STANDARD";
constexpr const char* kAllUsersUri  = "http://acs.amazonaws.com/groups/global/AllUsers";

/// A stable canonical user id, in the 64-hex-character shape S3 uses. Derived
/// from the access key so it survives restarts without being stored, and so it
/// does not leak the key itself into every listing.
std::string ownerId(const Config& config) {
    return sha256Hex(config.rootAccessKey);
}

void writeOwner(XmlWriter& writer, const Config& config) {
    writer.open("Owner");
    writer.element("ID", ownerId(config));
    writer.element("DisplayName", config.rootAccessKey);
    writer.close();
}

std::uint32_t parseMaxKeys(const S3Request& request) {
    const auto raw = request.queryValue("max-keys");
    if (!raw || raw->empty()) return limits::kDefaultMaxKeys;

    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(raw->data(), raw->data() + raw->size(), value);
    if (error != std::errc() || end != raw->data() + raw->size()) {
        throw S3Exception(S3ErrorCode::InvalidArgument, "max-keys must be a number.");
    }

    // S3 clamps rather than rejecting, and clients depend on that: several
    // ask for 100000 and expect a page of 1000 back.
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(value, limits::kMaxMaxKeys));
}

bool wantsUrlEncoding(const S3Request& request) {
    const auto encoding = request.queryValue("encoding-type");
    if (!encoding) return false;
    if (*encoding == "url") return true;
    throw S3Exception(S3ErrorCode::InvalidArgument, "encoding-type must be 'url'.");
}

/// The continuation token is the resumption key, base64-encoded. Opaque to the
/// client by contract, so encoding rather than signing it is enough — it grants
/// no access a plain `start-after` would not, and forging one only produces a
/// listing the same credentials could have asked for directly.
std::string encodeContinuation(std::string_view startAfter) {
    return base64Encode(startAfter);
}

std::string decodeContinuation(std::string_view token) {
    const auto decoded = base64Decode(token);
    if (!decoded) throw S3Exception(S3ErrorCode::InvalidArgument, "The continuation token is not valid.");
    return *decoded;
}

}  // namespace

void validateBucketPolicy(const std::string& document) {
    if (document.empty()) {
        throw S3Exception(S3ErrorCode::InvalidArgument, "The policy document is empty.");
    }
    if (document.size() > 20 * 1024) {
        throw S3Exception(S3ErrorCode::InvalidArgument,
                          "The policy document exceeds the maximum size of 20 KB.");
    }
    if (!nlohmann::json::accept(document)) {
        throw S3Exception(S3ErrorCode::InvalidArgument, "The policy document is not valid JSON.");
    }
}

// A document this does not recognise is stored and reported back unchanged but
// grants nothing — refusing to guess is the only safe direction to be wrong in.
bool policyGrantsAnonymousRead(const std::string& document, std::string_view bucket) {
    nlohmann::json policy;
    try {
        policy = nlohmann::json::parse(document);
    } catch (const nlohmann::json::exception&) {
        return false;
    }
    if (!policy.is_object() || !policy.contains("Statement")) return false;

    const auto& statements = policy["Statement"];
    if (!statements.is_array()) return false;

    const auto matches = [](const nlohmann::json& field, auto&& predicate) {
        if (field.is_string()) return predicate(field.get<std::string>());
        if (field.is_array()) {
            return std::any_of(field.begin(), field.end(), [&](const nlohmann::json& item) {
                return item.is_string() && predicate(item.get<std::string>());
            });
        }
        return false;
    };

    for (const auto& statement : statements) {
        if (!statement.is_object()) continue;
        if (statement.value("Effect", "") != "Allow") continue;

        // A statement carrying a Condition is not evaluated, so it must not be
        // treated as an unconditional grant.
        if (statement.contains("Condition")) continue;

        if (!statement.contains("Principal")) continue;
        const auto& principal = statement["Principal"];
        bool        anonymous = false;
        if (principal.is_string()) {
            anonymous = principal.get<std::string>() == "*";
        } else if (principal.is_object() && principal.contains("AWS")) {
            anonymous = matches(principal["AWS"], [](const std::string& value) {
                return value == "*";
            });
        }
        if (!anonymous) continue;

        if (!statement.contains("Action")) continue;
        const bool reads = matches(statement["Action"], [](const std::string& value) {
            return value == "s3:GetObject" || value == "s3:*" || value == "*";
        });
        if (!reads) continue;

        if (!statement.contains("Resource")) continue;
        const std::string prefix = "arn:aws:s3:::" + std::string(bucket);
        const bool        covers = matches(statement["Resource"], [&prefix](const std::string& value) {
            return value == prefix + "/*" || value == prefix + "*" || value == "*";
        });
        if (covers) return true;
    }

    return false;
}

drogon::HttpResponsePtr handleListBuckets(const S3Context& context, const S3Request& request) {
    XmlWriter writer("ListAllMyBucketsResult");
    writeOwner(writer, context.config);

    writer.open("Buckets");
    for (const auto& bucket : context.storage.listBuckets()) {
        writer.open("Bucket");
        writer.element("Name", bucket.name);
        writer.element("CreationDate", toIso8601(bucket.createdAt));
        writer.close();
    }
    writer.close();

    auto response = xmlResponse(writer.finish());
    applyCommonHeaders(response, request.requestId);
    return response;
}

drogon::HttpResponsePtr handleCreateBucket(const S3Context& context, const S3Request& request) {
    if (!isValidBucketName(request.bucket)) throw S3Exception(S3ErrorCode::InvalidBucketName);

    if (isReservedBucketName(request.bucket)) {
        // Drogon matches an exact path before the catch-all, so a bucket with
        // one of these names would exist in the metadata store and be
        // unreachable over HTTP. Refusing it here is the difference between a
        // clear error now and an unexplainable 404 later.
        throw S3Exception(S3ErrorCode::InvalidBucketName,
                          "'" + request.bucket +
                              "' is reserved for a server endpoint and cannot be used as a "
                              "bucket name.");
    }

    try {
        context.storage.createBucket(request.bucket);
    } catch (const StorageError& error) {
        if (error.code() != StorageErrorCode::BucketAlreadyExists) throw;

        // S3's own inconsistency, reproduced because clients script around it:
        // in us-east-1 recreating a bucket you own succeeds, and everywhere
        // else it is a conflict.
        if (context.config.region != "us-east-1") {
            throw S3Exception(S3ErrorCode::BucketAlreadyOwnedByYou);
        }
    }

    invalidateBucket(context, request.bucket);

    auto response = emptyResponse(200);
    response->addHeader("Location", "/" + request.bucket);
    applyCommonHeaders(response, request.requestId);
    return response;
}

drogon::HttpResponsePtr handleDeleteBucket(const S3Context& context, const S3Request& request) {
    context.storage.deleteBucket(request.bucket);
    invalidateBucket(context, request.bucket);

    auto response = emptyResponse(204);
    applyCommonHeaders(response, request.requestId);
    return response;
}

drogon::HttpResponsePtr handleHeadBucket(const S3Context& context, const S3Request& request) {
    requireBucket(context, request.bucket);

    auto response = emptyResponse(200);
    response->addHeader("x-amz-bucket-region", context.config.region);
    applyCommonHeaders(response, request.requestId);
    return response;
}

drogon::HttpResponsePtr handleListObjects(const S3Context& context, const S3Request& request,
                                          bool version2) {
    requireBucket(context, request.bucket);

    const bool urlEncode = wantsUrlEncoding(request);

    ListObjectsRequest query;
    query.prefix    = request.queryOr("prefix");
    query.delimiter = request.queryOr("delimiter");
    query.maxKeys   = parseMaxKeys(request);

    std::string continuationToken;
    std::string startAfter;
    std::string marker;

    if (version2) {
        continuationToken = request.queryOr("continuation-token");
        startAfter        = request.queryOr("start-after");
        // A continuation token always wins: it is the server's own resumption
        // point, and start-after is the client's initial guess at one.
        query.startAfter = continuationToken.empty() ? startAfter
                                                     : decodeContinuation(continuationToken);
    } else {
        marker           = request.queryOr("marker");
        query.startAfter = marker;
    }

    // max-keys=0 is a legal way to ask "does this prefix match anything", and
    // the engine treats 0 as "use the default", so the two are separated here.
    const bool empty = query.maxKeys == 0;
    if (empty) query.maxKeys = 1;

    ListObjectsResult result;
    if (!empty) {
        result = context.storage.listObjects(request.bucket, query);
    }

    XmlWriter writer("ListBucketResult");
    writer.element("Name", request.bucket);
    writer.element("Prefix", encodeListingValue(query.prefix, urlEncode));

    if (version2) {
        writer.elementIfSet("ContinuationToken", continuationToken);
        writer.elementIfSet("StartAfter", encodeListingValue(startAfter, urlEncode));
    } else {
        writer.element("Marker", encodeListingValue(marker, urlEncode));
    }

    writer.element("MaxKeys", static_cast<std::uint64_t>(empty ? 0 : query.maxKeys));
    writer.elementIfSet("Delimiter", encodeListingValue(query.delimiter, urlEncode));
    if (urlEncode) writer.element("EncodingType", "url");
    writer.booleanElement("IsTruncated", result.truncated);

    if (version2) {
        writer.element("KeyCount",
                       static_cast<std::uint64_t>(result.objects.size() +
                                                  result.commonPrefixes.size()));
        if (result.truncated) {
            writer.element("NextContinuationToken", encodeContinuation(result.nextStartAfter));
        }
    } else if (result.truncated) {
        // V1 only sends NextMarker when a delimiter is in play; otherwise the
        // client is expected to use the last key it received. Sending it
        // unconditionally is harmless and saves clients that read it anyway.
        writer.element("NextMarker", encodeListingValue(result.nextStartAfter, urlEncode));
    }

    const bool fetchOwner = !version2 || request.queryOr("fetch-owner") == "true";

    for (const auto& object : result.objects) {
        writer.open("Contents");
        writer.element("Key", encodeListingValue(object.key, urlEncode));
        writer.element("LastModified", toIso8601(object.lastModified));
        writer.element("ETag", quoteETag(object.etag));
        writer.element("Size", object.size);
        writer.element("StorageClass", kStorageClass);
        if (fetchOwner) writeOwner(writer, context.config);
        writer.close();
    }

    for (const auto& prefix : result.commonPrefixes) {
        writer.open("CommonPrefixes");
        writer.element("Prefix", encodeListingValue(prefix, urlEncode));
        writer.close();
    }

    auto response = xmlResponse(writer.finish());
    applyCommonHeaders(response, request.requestId);
    return response;
}

drogon::HttpResponsePtr handleGetBucketLocation(const S3Context& context,
                                                const S3Request& request) {
    requireBucket(context, request.bucket);

    // The one document whose payload is the root element's own text, so it is
    // assembled rather than built with XmlWriter. us-east-1 is reported as an
    // empty element — that is not an omission, it is what the AWS SDKs expect
    // and what they translate back into "us-east-1".
    const std::string region =
        context.config.region == "us-east-1" ? std::string() : xmlEscape(context.config.region);

    auto response = xmlResponse(std::string(R"(<?xml version="1.0" encoding="UTF-8"?>)") +
                                "\n<LocationConstraint xmlns=\"" + kS3Namespace + "\">" + region +
                                "</LocationConstraint>\n");
    applyCommonHeaders(response, request.requestId);
    return response;
}

drogon::HttpResponsePtr handleGetBucketVersioning(const S3Context& context,
                                                  const S3Request& request) {
    requireBucket(context, request.bucket);

    // An empty configuration means versioning has never been enabled, which is
    // both true and what every client reads as "not versioned".
    XmlWriter writer("VersioningConfiguration");
    auto      response = xmlResponse(writer.finish());
    applyCommonHeaders(response, request.requestId);
    return response;
}

drogon::HttpResponsePtr handleGetBucketAcl(const S3Context& context, const S3Request& request) {
    const BucketRecord bucket = requireBucket(context, request.bucket);

    XmlWriter writer("AccessControlPolicy");
    writeOwner(writer, context.config);
    writer.open("AccessControlList");

    writer.open("Grant");
    writer.open("Grantee");
    writer.element("ID", ownerId(context.config));
    writer.element("DisplayName", context.config.rootAccessKey);
    writer.close();
    writer.element("Permission", "FULL_CONTROL");
    writer.close();

    if (bucket.publicRead) {
        writer.open("Grant");
        writer.open("Grantee");
        writer.element("URI", kAllUsersUri);
        writer.close();
        writer.element("Permission", "READ");
        writer.close();
    }

    writer.close();

    auto response = xmlResponse(writer.finish());
    applyCommonHeaders(response, request.requestId);
    return response;
}

drogon::HttpResponsePtr handlePutBucketAcl(const S3Context& context, const S3Request& request,
                                           const drogon::HttpRequestPtr& http,
                                           const S3Body& body) {
    requireBucket(context, request.bucket);

    // Only the two grants this server can actually represent are accepted. A
    // canned ACL naming something finer — authenticated-read, for instance —
    // would be stored as "public" or "private" and mean neither.
    bool publicRead = false;
    bool decided    = false;

    if (const std::string& canned = http->getHeader("x-amz-acl"); !canned.empty()) {
        if (canned == "public-read" || canned == "public-read-write") {
            publicRead = true;
            decided    = true;
        } else if (canned == "private") {
            decided = true;
        } else {
            throw S3Exception(S3ErrorCode::NotImplemented,
                              "Only the 'private' and 'public-read' canned ACLs are supported.");
        }
    }

    if (!decided) {
        const std::string document = body.materialise();
        if (document.empty()) {
            throw S3Exception(S3ErrorCode::InvalidRequest,
                              "PutBucketAcl requires an x-amz-acl header or an ACL document.");
        }

        XmlNode root;
        try {
            root = parseXml(document);
        } catch (const XmlParseError& error) {
            throw S3Exception(S3ErrorCode::MalformedXML, error.what());
        }

        const XmlNode* list = root.child("AccessControlList");
        if (list != nullptr) {
            for (const XmlNode* grant : list->childrenNamed("Grant")) {
                const XmlNode* grantee = grant->child("Grantee");
                if (grantee == nullptr) continue;
                if (grantee->childText("URI") != kAllUsersUri) continue;

                const std::string permission = grant->childText("Permission");
                if (permission == "READ" || permission == "FULL_CONTROL") publicRead = true;
            }
        }
    }

    context.storage.setBucketPublicRead(request.bucket, publicRead);
    invalidateBucket(context, request.bucket);

    auto response = emptyResponse(200);
    applyCommonHeaders(response, request.requestId);
    return response;
}

drogon::HttpResponsePtr handleGetBucketPolicy(const S3Context& context, const S3Request& request) {
    const BucketRecord bucket = requireBucket(context, request.bucket);
    if (bucket.policy.empty()) throw S3Exception(S3ErrorCode::NoSuchBucketPolicy);

    // The policy is returned as JSON, not XML — the one S3 endpoint that is.
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k200OK);
    response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    response->setBody(bucket.policy);
    applyCommonHeaders(response, request.requestId);
    return response;
}

drogon::HttpResponsePtr handlePutBucketPolicy(const S3Context& context, const S3Request& request,
                                              const S3Body& body) {
    requireBucket(context, request.bucket);

    const std::string document = body.materialise();
    validateBucketPolicy(document);

    const bool publicRead = policyGrantsAnonymousRead(document, request.bucket);
    context.storage.setBucketPolicy(request.bucket, document, publicRead);
    invalidateBucket(context, request.bucket);

    auto response = emptyResponse(204);
    applyCommonHeaders(response, request.requestId);
    return response;
}

drogon::HttpResponsePtr handleDeleteBucketPolicy(const S3Context& context,
                                                 const S3Request& request) {
    requireBucket(context, request.bucket);

    // Removing the policy removes the anonymous access it granted. Leaving the
    // flag set would keep a bucket public with nothing left to say why.
    context.storage.setBucketPolicy(request.bucket, std::string(), false);
    invalidateBucket(context, request.bucket);

    auto response = emptyResponse(204);
    applyCommonHeaders(response, request.requestId);
    return response;
}

}  // namespace monobucket::s3
