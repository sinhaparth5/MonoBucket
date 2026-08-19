#include "s3/handlers.hpp"

#include <algorithm>
#include <array>
#include <cctype>

#include "cache/cache_provider.hpp"
#include "core/config.hpp"
#include "s3/base64.hpp"
#include "s3/chunked.hpp"
#include "s3/s3_error.hpp"
#include "storage/codec.hpp"
#include "storage/digest.hpp"
#include "storage/storage_engine.hpp"

namespace monobucket::s3 {
namespace {

/// S3 caps the whole user-metadata header block at 2 KB.
constexpr std::size_t kMaxUserMetadataBytes = 2048;

/// Version byte for cached object records. Independent of the storage record's
/// own version on purpose: a cache entry is disposable, so this can change
/// without a migration, and coupling the two would make it look as though it
/// could not.
constexpr std::uint8_t kCachedObjectVersion = 2;

std::string encodeObject(const ObjectRecord& record) {
    std::string  out;
    codec::Writer writer(out);
    writer.u8(kCachedObjectVersion);
    writer.string(record.key);
    writer.string(record.blobId);
    writer.varint(record.size);
    writer.string(record.etag);
    writer.string(record.sha256);
    writer.string(record.contentType);
    writer.varint(static_cast<std::uint64_t>(record.lastModified));
    writer.varint(record.userMetadata.size());
    for (const auto& [name, value] : record.userMetadata) {
        writer.string(name);
        writer.string(value);
    }
    // Cached alongside the rest rather than fetched from the store on demand.
    // A cached read that quietly dropped the checksum would answer a
    // checksum-mode GET differently depending on whether the entry was warm,
    // which is the one thing a cache must never do.
    writer.u8(record.checksum.algorithm
                  ? static_cast<std::uint8_t>(*record.checksum.algorithm) + 1
                  : 0);
    if (record.checksum.algorithm) {
        writer.string(record.checksum.value);
        writer.varint(record.checksum.parts);
    }
    return out;
}

std::optional<ObjectRecord> decodeObject(std::string_view stored) {
    try {
        codec::Reader reader(stored);
        if (reader.u8() != kCachedObjectVersion) return std::nullopt;

        ObjectRecord record;
        record.key          = reader.string();
        record.blobId       = reader.string();
        record.size         = reader.varint();
        record.etag         = reader.string();
        record.sha256       = reader.string();
        record.contentType  = reader.string();
        record.lastModified = static_cast<TimestampMs>(reader.varint());

        const std::uint64_t count = reader.varint();
        for (std::uint64_t i = 0; i < count; ++i) {
            std::string name = reader.string();
            record.userMetadata.emplace(std::move(name), reader.string());
        }

        if (const std::uint8_t tag = reader.u8(); tag != 0) {
            if (tag > static_cast<std::uint8_t>(ChecksumAlgorithm::Sha256) + 1) {
                return std::nullopt;
            }
            record.checksum.algorithm = static_cast<ChecksumAlgorithm>(tag - 1);
            record.checksum.value     = reader.string();
            record.checksum.parts     = static_cast<std::uint32_t>(reader.varint());
        }
        return record;
    } catch (const codec::DecodeError&) {
        // A cache entry we cannot read is not an error: drop it and go to the
        // store. This is the whole reason the cache format is versioned
        // separately — a rolling upgrade reads the other version's entries.
        return std::nullopt;
    }
}

}  // namespace

// --- Body ------------------------------------------------------------------

S3Body::S3Body(std::string_view raw, const AuthOutcome& auth, std::string_view contentEncoding,
               std::uint64_t decodedLength)
    : raw_(raw), auth_(auth), decodedLength_(decodedLength) {
    // The framing is decided by the payload marker, not by Content-Encoding
    // alone: at least one SDK sends `aws-chunked` on a body it did not frame,
    // and trusting the header over the signature would let a client smuggle a
    // chunk header into the object.
    chunked_ = auth_.payload == PayloadMode::StreamingSigned ||
               auth_.payload == PayloadMode::StreamingUnsigned;

    if (!chunked_ && isAwsChunked(contentEncoding) && decodedLength > 0) {
        // Framed but signed as a whole. Rare, but legal, and the framing still
        // has to come off before the bytes are stored.
        chunked_ = true;
    }
}

void S3Body::streamTo(const std::function<void(std::string_view)>& sink) const {
    if (chunked_) {
        ChunkedDecoder::Options options;
        options.verifySignatures = auth_.payload == PayloadMode::StreamingSigned;
        options.signingKey       = auth_.signingKey;
        options.amzDate          = auth_.amzDate;
        options.scope            = auth_.scope;
        options.seedSignature    = auth_.signature;
        options.expectTrailerSignature = auth_.trailerExpected;
        options.declaredLength   = decodedLength_;

        ChunkedDecoder decoder(std::move(options));
        decoder.decode(raw_, sink);
        trailers_ = decoder.trailers();
        return;
    }

    if (auth_.payload == PayloadMode::Signed) {
        // Hashed before a byte is written rather than checked afterwards: a
        // digest verified after the object is published is a digest that let a
        // corrupt object be visible, however briefly.
        if (!secureEquals(sha256Hex(raw_), auth_.payloadSha256)) {
            throw S3Exception(S3ErrorCode::XAmzContentSHA256Mismatch);
        }
    }

    sink(raw_);
}

std::string S3Body::materialise() const {
    std::string out;
    out.reserve(raw_.size());
    streamTo([&out](std::string_view chunk) { out.append(chunk); });
    return out;
}

// --- Shared lookups --------------------------------------------------------

BucketRecord requireBucket(const S3Context& context, std::string_view name) {
    if (!isValidBucketName(name)) throw S3Exception(S3ErrorCode::InvalidBucketName);

    const std::string cacheKey = bucketCacheKey(name);
    if (const CacheValuePtr cached = context.cache.get(cacheKey)) {
        codec::Reader reader(*cached);
        try {
            BucketRecord record;
            record.name       = std::string(name);
            record.createdAt  = static_cast<TimestampMs>(reader.varint());
            record.publicRead = reader.boolean();
            record.policy     = reader.string();
            record.cors       = decodeCorsRules(reader);
            return record;
        } catch (const codec::DecodeError&) {
            // Fall through to the store.
        }
    }

    auto record = context.storage.getBucket(name);
    if (!record) throw S3Exception(S3ErrorCode::NoSuchBucket);

    std::string   encoded;
    codec::Writer writer(encoded);
    writer.varint(static_cast<std::uint64_t>(record->createdAt));
    writer.boolean(record->publicRead);
    writer.string(record->policy);
    encodeCorsRules(writer, record->cors);
    context.cache.put(cacheKey, encoded, std::chrono::seconds(context.config.cacheTtlSeconds));

    return *record;
}

std::optional<ObjectRecord> statObject(const S3Context& context, std::string_view bucket,
                                       std::string_view key) {
    const std::string cacheKey = objectCacheKey(bucket, key);

    if (const CacheValuePtr cached = context.cache.get(cacheKey)) {
        if (auto record = decodeObject(*cached)) return record;
    }

    auto record = context.storage.statObject(bucket, key);
    if (!record) {
        // Deliberately no negative caching. A miss is cheap — one RocksDB point
        // lookup that the bloom filter usually answers — and caching it would
        // put read-after-write consistency at the mercy of the cache TTL for
        // the one case where a client is most likely to retry immediately.
        return std::nullopt;
    }

    context.cache.put(cacheKey, encodeObject(*record),
                      std::chrono::seconds(context.config.cacheTtlSeconds));
    return record;
}

void invalidateObject(const S3Context& context, std::string_view bucket, std::string_view key) {
    context.cache.del(objectCacheKey(bucket, key));
}

void invalidateBucket(const S3Context& context, std::string_view name) {
    context.cache.del(bucketCacheKey(name));
}

std::string bucketCacheKey(std::string_view name) {
    return "b:" + std::string(name);
}

std::string objectCacheKey(std::string_view bucket, std::string_view key) {
    std::string out;
    out.reserve(bucket.size() + key.size() + 3);
    out += "o:";
    out += bucket;
    out += '/';
    out += key;
    return out;
}

// --- Header helpers --------------------------------------------------------

UserMetadata collectUserMetadata(const drogon::HttpRequestPtr& request) {
    UserMetadata metadata;
    std::size_t  bytes = 0;

    for (const auto& [name, value] : request->getHeaders()) {
        const std::string key = userMetadataKey(name);
        if (key.empty()) continue;

        bytes += key.size() + value.size();
        if (bytes > kMaxUserMetadataBytes) {
            throw S3Exception(S3ErrorCode::InvalidArgument,
                              "Your metadata headers exceed the maximum allowed metadata size.");
        }
        metadata.emplace(key, value);
    }
    return metadata;
}

std::string contentTypeOf(const drogon::HttpRequestPtr& request) {
    const std::string& value = request->getHeader("content-type");
    if (value.empty()) return "application/octet-stream";

    // `aws-chunked` uploads sometimes arrive with the SDK's own placeholder;
    // storing it would make every streamed object claim to be a form.
    if (value == "application/x-www-form-urlencoded") return "application/octet-stream";
    return value;
}

namespace {

/// Compares a `Content-MD5` header against an MD5 already computed in hex.
/// Split out because the streaming path and the in-memory path arrive at the
/// same digest by different routes and must judge it the same way.
void checkContentMd5(const std::string& header, std::string_view actualHex) {
    const auto expected = base64Decode(header);
    if (!expected || expected->size() != 16) throw S3Exception(S3ErrorCode::InvalidDigest);

    const std::string expectedHex =
        toHex(std::span<const unsigned char>(
            reinterpret_cast<const unsigned char*>(expected->data()), expected->size()));

    if (!secureEquals(actualHex, expectedHex)) throw S3Exception(S3ErrorCode::BadDigest);
}

}  // namespace

void verifyContentMd5(const drogon::HttpRequestPtr& request, std::string_view payload) {
    const std::string& header = request->getHeader("content-md5");
    if (header.empty()) return;
    checkContentMd5(header, md5Hex(payload));
}

ChecksumHeaders checksumHeaders(const drogon::HttpRequestPtr& request) {
    ChecksumHeaders out;
    for (const auto& [name, value] : request->getHeaders()) {
        std::string lowered = name;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (lowered.rfind("x-amz-checksum-", 0) == 0 || lowered == "x-amz-trailer" ||
            lowered == "x-amz-sdk-checksum-algorithm") {
            out.emplace(std::move(lowered), value);
        }
    }
    return out;
}

Checksum verifiedChecksum(const drogon::HttpRequestPtr& request, const ChecksumRequest& wanted,
                          const S3Body& body, const std::function<void(std::string_view)>& sink) {
    const std::string& contentMd5 = request->getHeader("content-md5");

    std::optional<ChecksumComputer> computer;
    if (wanted.wanted()) computer.emplace(*wanted.algorithm);

    // The blob writer computes an MD5 of its own — it is the ETag — but that
    // one is not finalised until the payload has been committed, and this has
    // to be able to refuse before then. A second pass is the price, and it is
    // paid only by the requests that ask for it.
    std::optional<Digest> md5;
    if (!contentMd5.empty()) md5.emplace();

    if (!computer && !md5) {
        body.streamTo(sink);
        return {};
    }

    body.streamTo([&computer, &md5, &sink](std::string_view chunk) {
        if (computer) computer->update(chunk);
        if (md5) md5->update(chunk);
        sink(chunk);
    });

    if (md5) checkContentMd5(contentMd5, md5->finish().md5);
    if (!computer) return {};

    Checksum computed;
    computed.algorithm = wanted.algorithm;
    computed.value     = computer->finish();

    // The expected value, wherever the client put it. A trailer is only
    // readable now, which is why this runs after the stream and still before
    // the commit — the payload is in a temporary that nothing references.
    std::string expected = wanted.expected;
    if (wanted.inTrailer) {
        const auto& trailers = body.trailers();
        const auto  found    = trailers.find(wanted.trailerName);
        if (found == trailers.end()) {
            throw S3Exception(S3ErrorCode::InvalidRequest,
                              "x-amz-trailer announced " + wanted.trailerName +
                                  ", but the body ended without it.");
        }
        expected = found->second;
    }

    // An algorithm named without a value is not a claim about the bytes — it is
    // CreateMultipartUpload deciding what the parts will carry, or an SDK
    // announcing an algorithm it then computed for us. There is nothing to
    // disagree with, so the value is stored and the object stands.
    if (!expected.empty()) verifyChecksum(*wanted.algorithm, expected, computed.value);
    return computed;
}

void applyChecksumHeader(const drogon::HttpResponsePtr& response, const Checksum& checksum) {
    if (!checksum.present()) return;
    response->addHeader(checksumHeaderName(*checksum.algorithm), encodeChecksum(checksum));
}

}  // namespace monobucket::s3
