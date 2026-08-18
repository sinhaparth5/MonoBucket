#include "s3/sigv4.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <ctime>

#include <openssl/hmac.h>

#include "s3/s3_error.hpp"
#include "storage/digest.hpp"

namespace monobucket::s3 {
namespace {

std::string toLower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// Trims the ends and collapses internal runs of whitespace, which is what the
/// specification means by "trimall".
std::string canonicalHeaderValue(std::string_view value) {
    std::string out;
    out.reserve(value.size());

    bool pendingSpace = false;
    for (const char ch : value) {
        const bool space = (ch == ' ' || ch == '\t');
        if (space) {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(' ');
            pendingSpace = false;
        }
        out.push_back(ch);
    }
    return out;
}

/// Splits `host;x-amz-date` and lowercases each name.
std::vector<std::string> splitSignedHeaders(std::string_view list) {
    std::vector<std::string> names;
    std::size_t              pos = 0;
    while (pos <= list.size()) {
        const std::size_t sep = list.find(';', pos);
        const std::size_t end = (sep == std::string_view::npos) ? list.size() : sep;
        if (end > pos) names.push_back(toLower(list.substr(pos, end - pos)));
        if (sep == std::string_view::npos) break;
        pos = sep + 1;
    }
    return names;
}

/// `Credential=AK/20260816/us-east-1/s3/aws4_request` split into its parts.
struct CredentialScope {
    std::string accessKey;
    std::string dateStamp;
    std::string region;
    std::string service;
    std::string terminator;

    /// Everything after the access key, which is what the string-to-sign uses.
    std::string scope() const {
        return dateStamp + '/' + region + '/' + service + '/' + terminator;
    }
};

CredentialScope parseCredential(std::string_view text) {
    std::array<std::string, 5> parts;
    std::size_t                index = 0;
    std::size_t                pos   = 0;

    while (index < parts.size()) {
        const std::size_t sep = text.find('/', pos);
        const std::size_t end = (sep == std::string_view::npos) ? text.size() : sep;
        parts[index++] = std::string(text.substr(pos, end - pos));
        if (sep == std::string_view::npos) break;
        pos = sep + 1;
    }

    if (index != 5 || pos > text.size()) {
        throw S3Exception(S3ErrorCode::InvalidArgument,
                          "Credential must be of the form "
                          "<access-key>/<date>/<region>/s3/aws4_request");
    }

    CredentialScope scope;
    scope.accessKey  = parts[0];
    scope.dateStamp  = parts[1];
    scope.region     = parts[2];
    scope.service    = parts[3];
    scope.terminator = parts[4];
    return scope;
}

/// Parses `Credential=..., SignedHeaders=..., Signature=...`.
///
/// Separators are lenient about surrounding whitespace because clients differ:
/// the AWS SDKs emit `, `, some older ones emit `,`, and at least one emits no
/// space after `Credential=`.
struct AuthorizationHeader {
    std::string credential;
    std::string signedHeaders;
    std::string signature;
};

AuthorizationHeader parseAuthorization(std::string_view value) {
    constexpr std::string_view kPrefix = "AWS4-HMAC-SHA256";
    if (value.compare(0, kPrefix.size(), kPrefix) != 0) {
        throw S3Exception(S3ErrorCode::InvalidRequest,
                          "Only AWS4-HMAC-SHA256 authorization is supported.");
    }
    value.remove_prefix(kPrefix.size());

    AuthorizationHeader header;
    std::size_t         pos = 0;
    while (pos < value.size()) {
        while (pos < value.size() && (value[pos] == ' ' || value[pos] == ',')) ++pos;
        if (pos >= value.size()) break;

        const std::size_t eq = value.find('=', pos);
        if (eq == std::string_view::npos) break;

        std::size_t end = value.find(',', eq);
        if (end == std::string_view::npos) end = value.size();

        const std::string_view name  = value.substr(pos, eq - pos);
        std::string_view       field = value.substr(eq + 1, end - eq - 1);
        while (!field.empty() && field.back() == ' ') field.remove_suffix(1);

        if (name == "Credential")        header.credential    = std::string(field);
        else if (name == "SignedHeaders") header.signedHeaders = std::string(field);
        else if (name == "Signature")     header.signature     = std::string(field);

        pos = end + 1;
    }

    if (header.credential.empty() || header.signedHeaders.empty() || header.signature.empty()) {
        throw S3Exception(S3ErrorCode::InvalidRequest,
                          "Authorization header is missing Credential, SignedHeaders or "
                          "Signature.");
    }
    return header;
}

PayloadMode payloadModeFor(std::string_view contentSha256, std::string& digestOut) {
    if (contentSha256 == kUnsignedPayload) return PayloadMode::Unsigned;
    if (contentSha256 == kStreamingSigned || contentSha256 == kStreamingSignedTrailer) {
        return PayloadMode::StreamingSigned;
    }
    if (contentSha256 == kStreamingUnsignedTrailer) return PayloadMode::StreamingUnsigned;

    if (contentSha256.size() != 64 ||
        !std::all_of(contentSha256.begin(), contentSha256.end(), [](unsigned char c) {
            return std::isxdigit(c) != 0;
        })) {
        throw S3Exception(S3ErrorCode::InvalidArgument,
                          "x-amz-content-sha256 must be a hex digest, UNSIGNED-PAYLOAD or one of "
                          "the streaming payload markers.");
    }

    digestOut = toLower(contentSha256);
    return PayloadMode::Signed;
}

/// Digests the string-to-sign under the derived key and returns lowercase hex.
std::string sign(std::string_view signingKey, std::string_view toSign) {
    const std::string raw = hmacSha256(signingKey, toSign);
    return toHex(std::span<const unsigned char>(
        reinterpret_cast<const unsigned char*>(raw.data()), raw.size()));
}

}  // namespace

// --- SigningRequest --------------------------------------------------------

std::string_view SigningRequest::header(std::string_view name) const {
    for (const auto& [key, value] : headers) {
        if (key == name) return value;
    }
    return {};
}

bool SigningRequest::hasHeader(std::string_view name) const {
    return std::any_of(headers.begin(), headers.end(),
                       [name](const auto& entry) { return entry.first == name; });
}

std::string CanonicalRequest::render() const {
    std::string out;
    out.reserve(uri.size() + query.size() + headers.size() + 128);
    out += method;
    out += '\n';
    out += uri;
    out += '\n';
    out += query;
    out += '\n';
    out += headers;
    out += '\n';
    out += signedHeaders;
    out += '\n';
    out += payloadHash;
    return out;
}

// --- Primitives ------------------------------------------------------------

std::string hmacSha256(std::string_view key, std::string_view data) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int  length = 0;

    // A zero-length key is legal for HMAC but HMAC() rejects a null pointer, so
    // give it something to point at.
    static const unsigned char kEmpty = 0;
    const unsigned char* keyData =
        key.empty() ? &kEmpty : reinterpret_cast<const unsigned char*>(key.data());

    if (::HMAC(EVP_sha256(), keyData, static_cast<int>(key.size()),
               reinterpret_cast<const unsigned char*>(data.data()), data.size(), out,
               &length) == nullptr) {
        throw S3Exception(S3ErrorCode::InternalError, "HMAC-SHA256 failed");
    }
    return std::string(reinterpret_cast<const char*>(out), length);
}

std::string deriveSigningKey(std::string_view secret, std::string_view dateStamp,
                             std::string_view region, std::string_view service) {
    const std::string initial = "AWS4" + std::string(secret);
    const std::string date    = hmacSha256(initial, dateStamp);
    const std::string scoped  = hmacSha256(date, region);
    const std::string serviced = hmacSha256(scoped, service);
    return hmacSha256(serviced, kTerminator);
}

std::string stringToSign(std::string_view amzDate, std::string_view scope,
                         std::string_view canonicalRequestHash) {
    std::string out(kAlgorithm);
    out += '\n';
    out += amzDate;
    out += '\n';
    out += scope;
    out += '\n';
    out += canonicalRequestHash;
    return out;
}

CanonicalRequest buildCanonicalRequest(const SigningRequest& request,
                                       const std::vector<std::string>& signedHeaders,
                                       std::string_view payloadHash) {
    CanonicalRequest canonical;
    canonical.method      = request.method;
    canonical.uri         = request.uri.empty() ? "/" : request.uri;
    canonical.query       = canonicalQueryString(parseQuery(request.query), "X-Amz-Signature");
    canonical.payloadHash = payloadHash;

    std::vector<std::string> names = signedHeaders;
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());

    for (const auto& name : names) {
        if (!request.hasHeader(name)) {
            // The client signed a header it did not send. Reporting this as a
            // signature mismatch is what S3 does, and the message says which
            // header so the failure is diagnosable.
            throw S3Exception(S3ErrorCode::SignatureDoesNotMatch,
                              "The request signed the header '" + name +
                                  "', which was not present in the request.");
        }
        canonical.headers += name;
        canonical.headers += ':';
        canonical.headers += canonicalHeaderValue(request.header(name));
        canonical.headers += '\n';

        if (!canonical.signedHeaders.empty()) canonical.signedHeaders += ';';
        canonical.signedHeaders += name;
    }

    return canonical;
}

std::optional<std::int64_t> parseAmzDate(std::string_view text) {
    // 20260816T120000Z — the only form S3 accepts in x-amz-date.
    if (text.size() != 16 || text[8] != 'T' || text[15] != 'Z') return std::nullopt;

    const auto digits = [&text](std::size_t offset, std::size_t count) -> std::optional<int> {
        int value = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const unsigned char c = static_cast<unsigned char>(text[offset + i]);
            if (std::isdigit(c) == 0) return std::nullopt;
            value = value * 10 + (c - '0');
        }
        return value;
    };

    const auto year   = digits(0, 4);
    const auto month  = digits(4, 2);
    const auto day    = digits(6, 2);
    const auto hour   = digits(9, 2);
    const auto minute = digits(11, 2);
    const auto second = digits(13, 2);
    if (!year || !month || !day || !hour || !minute || !second) return std::nullopt;

    std::tm parts{};
    parts.tm_year = *year - 1900;
    parts.tm_mon  = *month - 1;
    parts.tm_mday = *day;
    parts.tm_hour = *hour;
    parts.tm_min  = *minute;
    parts.tm_sec  = *second;

#if defined(_WIN32)
    const std::time_t stamp = ::_mkgmtime(&parts);
#else
    const std::time_t stamp = ::timegm(&parts);
#endif
    if (stamp == static_cast<std::time_t>(-1)) return std::nullopt;

    // timegm normalises out-of-range fields rather than rejecting them, so
    // 20260230 would silently become 2 March. Comparing back catches that.
    if (parts.tm_year != *year - 1900 || parts.tm_mon != *month - 1 || parts.tm_mday != *day) {
        return std::nullopt;
    }

    return static_cast<std::int64_t>(stamp);
}

std::string formatAmzDate(std::int64_t seconds) {
    const std::time_t stamp = static_cast<std::time_t>(seconds);
    std::tm           parts{};
#if defined(_WIN32)
    ::gmtime_s(&parts, &stamp);
#else
    ::gmtime_r(&stamp, &parts);
#endif

    char buffer[17];
    std::strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%SZ", &parts);
    return std::string(buffer);
}

bool secureEquals(std::string_view a, std::string_view b) noexcept {
    // The lengths are public — signatures are fixed width — but the contents
    // are not, so the comparison runs over every byte regardless.
    if (a.size() != b.size()) return false;

    unsigned char difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        difference |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return difference == 0;
}

// --- Verification ----------------------------------------------------------

AuthOutcome authenticate(const SigningRequest& request, const std::vector<QueryParam>& query,
                         const Credentials& credentials, const AuthOptions& options) {
    return authenticate(request, query,
                        [&credentials](std::string_view accessKeyId)
                            -> std::optional<std::string> {
                            // Constant time even though an access key id is not
                            // a secret: a short-circuiting compare would let an
                            // attacker enumerate a valid id one character at a
                            // time, and the check costs nothing.
                            if (!secureEquals(accessKeyId, credentials.accessKey)) {
                                return std::nullopt;
                            }
                            return credentials.secretKey;
                        },
                        options);
}

AuthOutcome authenticate(const SigningRequest& request, const std::vector<QueryParam>& query,
                         const CredentialResolver& resolve, const AuthOptions& options) {
    const std::string_view authorization = request.header("authorization");
    const auto             presignedSignature = findQuery(query, "X-Amz-Signature");

    if (authorization.empty() && !presignedSignature.has_value()) {
        AuthOutcome outcome;
        outcome.anonymous = true;
        return outcome;
    }

    AuthOutcome outcome;
    std::string credential;
    std::string signedHeaderList;
    std::string providedSignature;
    std::string payloadHash;

    if (presignedSignature.has_value()) {
        // --- Presigned URL ---------------------------------------------------
        outcome.presigned = true;

        const auto algorithm = findQuery(query, "X-Amz-Algorithm");
        if (!algorithm || *algorithm != kAlgorithm) {
            throw S3Exception(S3ErrorCode::InvalidArgument,
                              "X-Amz-Algorithm must be AWS4-HMAC-SHA256.");
        }

        const auto credentialParam   = findQuery(query, "X-Amz-Credential");
        const auto dateParam         = findQuery(query, "X-Amz-Date");
        const auto signedHeaderParam = findQuery(query, "X-Amz-SignedHeaders");
        if (!credentialParam || !dateParam || !signedHeaderParam) {
            throw S3Exception(S3ErrorCode::InvalidArgument,
                              "A presigned URL requires X-Amz-Credential, X-Amz-Date and "
                              "X-Amz-SignedHeaders.");
        }

        credential        = *credentialParam;
        signedHeaderList  = *signedHeaderParam;
        providedSignature = *presignedSignature;
        outcome.amzDate   = *dateParam;

        // The body of a presigned request is never covered by the signature,
        // whatever x-amz-content-sha256 happens to say.
        payloadHash     = kUnsignedPayload;
        outcome.payload = PayloadMode::Unsigned;

        const auto signedAt = parseAmzDate(outcome.amzDate);
        if (!signedAt) {
            throw S3Exception(S3ErrorCode::InvalidArgument, "X-Amz-Date is not a valid timestamp.");
        }

        std::int64_t expires = 0;
        if (const auto expiresParam = findQuery(query, "X-Amz-Expires")) {
            try {
                expires = std::stoll(*expiresParam);
            } catch (const std::exception&) {
                expires = -1;
            }
        }
        // S3's own bounds. Zero is not "never expires"; it is a URL that was
        // already invalid when it was minted.
        if (expires <= 0 || expires > 604800) {
            throw S3Exception(S3ErrorCode::InvalidArgument,
                              "X-Amz-Expires must be between 1 and 604800 seconds.");
        }

        // A presigned URL carries its own lifetime, so the clock-skew window
        // does not apply to its age — only to a clock that is ahead of ours.
        if (options.nowSeconds > *signedAt + expires) {
            throw S3Exception(S3ErrorCode::AccessDenied, "Request has expired.");
        }
        if (*signedAt - options.nowSeconds > options.skewSeconds) {
            throw S3Exception(S3ErrorCode::AccessDenied,
                              "The presigned URL is not valid yet.");
        }
    } else {
        // --- Authorization header --------------------------------------------
        const AuthorizationHeader header = parseAuthorization(authorization);
        credential        = header.credential;
        signedHeaderList  = header.signedHeaders;
        providedSignature = header.signature;

        std::string_view amzDate = request.header("x-amz-date");
        if (amzDate.empty()) amzDate = request.header("date");
        if (amzDate.empty()) {
            throw S3Exception(S3ErrorCode::MissingSecurityHeader,
                              "A signed request must carry x-amz-date.");
        }
        outcome.amzDate = std::string(amzDate);

        const auto signedAt = parseAmzDate(outcome.amzDate);
        if (!signedAt) {
            throw S3Exception(S3ErrorCode::InvalidArgument, "x-amz-date is not a valid timestamp.");
        }

        const std::int64_t drift = options.nowSeconds > *signedAt ? options.nowSeconds - *signedAt
                                                                  : *signedAt - options.nowSeconds;
        if (drift > options.skewSeconds) {
            throw S3Exception(S3ErrorCode::RequestTimeTooSkewed,
                              "The difference between the request time and the server time is "
                              "greater than " +
                                  std::to_string(options.skewSeconds) + " seconds.");
        }

        std::string_view contentSha256 = request.header("x-amz-content-sha256");
        if (contentSha256.empty()) {
            // S3 requires the header on a signed request. Defaulting to
            // UNSIGNED-PAYLOAD would let a client opt out of body integrity by
            // omission, which is exactly backwards.
            throw S3Exception(S3ErrorCode::MissingSecurityHeader,
                              "A signed request must carry x-amz-content-sha256.");
        }
        outcome.payload = payloadModeFor(contentSha256, outcome.payloadSha256);
        payloadHash     = std::string(contentSha256);
    }

    const CredentialScope scope = parseCredential(credential);

    if (scope.service != kService || scope.terminator != kTerminator) {
        throw S3Exception(S3ErrorCode::InvalidArgument,
                          "Credential scope must end in /s3/aws4_request.");
    }
    if (outcome.amzDate.compare(0, 8, scope.dateStamp) != 0) {
        throw S3Exception(S3ErrorCode::InvalidArgument,
                          "The date in the credential scope does not match the request date.");
    }

    // A resolver that misses and one that finds a revoked key are the same
    // answer: nothing. The caller is told InvalidAccessKeyId either way, so a
    // revoked credential does not report itself as having once been real.
    const std::optional<std::string> secretKey = resolve(scope.accessKey);
    if (!secretKey) {
        throw S3Exception(S3ErrorCode::InvalidAccessKeyId);
    }
    outcome.accessKey = scope.accessKey;

    const std::vector<std::string> signedHeaders = splitSignedHeaders(signedHeaderList);
    if (std::find(signedHeaders.begin(), signedHeaders.end(), "host") == signedHeaders.end()) {
        // Without host in the signature the same signature is valid against any
        // endpoint the credentials reach.
        throw S3Exception(S3ErrorCode::SignatureDoesNotMatch,
                          "The 'host' header must be signed.");
    }

    outcome.scope      = scope.scope();
    outcome.signingKey =
        deriveSigningKey(*secretKey, scope.dateStamp, scope.region, scope.service);

    const std::string expected = toLower(providedSignature);

    // The method is the only part of the canonical request we may be unsure of,
    // so it is the only thing that varies between attempts.
    const auto attempt = [&](const std::string& method) {
        SigningRequest candidate = request;
        candidate.method         = method;

        const CanonicalRequest canonical =
            buildCanonicalRequest(candidate, signedHeaders, payloadHash);
        return sign(outcome.signingKey,
                    stringToSign(outcome.amzDate, outcome.scope, sha256Hex(canonical.render())));
    };

    outcome.signature = attempt(request.method);
    outcome.method    = request.method;

    if (!secureEquals(outcome.signature, expected)) {
        if (options.alternateMethod.empty()) throw S3Exception(S3ErrorCode::SignatureDoesNotMatch);

        outcome.signature = attempt(options.alternateMethod);
        outcome.method    = options.alternateMethod;

        if (!secureEquals(outcome.signature, expected)) {
            throw S3Exception(S3ErrorCode::SignatureDoesNotMatch);
        }
    }

    return outcome;
}

// --- Presigning ------------------------------------------------------------

std::string presignQuery(const PresignRequest& request, const Credentials& credentials) {
    if (request.expiresSeconds <= 0 || request.expiresSeconds > 604800) {
        throw S3Exception(S3ErrorCode::InvalidArgument,
                          "X-Amz-Expires must be between 1 and 604800 seconds.");
    }
    if (request.host.empty()) {
        // authenticate() refuses a signature that does not cover a host, so a
        // URL minted without one could never be redeemed.
        throw S3Exception(S3ErrorCode::InvalidArgument,
                          "A presigned URL must be bound to a host.");
    }

    const std::string amzDate   = formatAmzDate(request.nowSeconds);
    const std::string dateStamp = amzDate.substr(0, 8);
    const std::string scope =
        dateStamp + '/' + request.region + '/' + kService + '/' + kTerminator;

    std::vector<QueryParam> params;
    const auto              add = [&params](std::string name, std::string value) {
        params.push_back(QueryParam{std::move(name), std::move(value), true});
    };
    add("X-Amz-Algorithm", kAlgorithm);
    add("X-Amz-Credential", credentials.accessKey + '/' + scope);
    add("X-Amz-Date", amzDate);
    add("X-Amz-Expires", std::to_string(request.expiresSeconds));
    add("X-Amz-SignedHeaders", "host");

    // The canonical query string is also the one that goes in the URL. Building
    // it once means the bytes signed and the bytes sent cannot disagree about
    // ordering or encoding.
    const std::string query = canonicalQueryString(params);

    SigningRequest signing;
    signing.method = request.method;
    signing.uri    = request.uri;
    signing.query  = query;
    signing.headers.emplace_back("host", request.host);

    const CanonicalRequest canonical =
        buildCanonicalRequest(signing, {"host"}, kUnsignedPayload);
    const std::string signingKey =
        deriveSigningKey(credentials.secretKey, dateStamp, request.region, kService);

    return query + "&X-Amz-Signature=" +
           sign(signingKey, stringToSign(amzDate, scope, sha256Hex(canonical.render())));
}

std::string chunkSignature(std::string_view signingKey, std::string_view amzDate,
                           std::string_view scope, std::string_view previousSignature,
                           std::string_view chunkSha256Hex) {
    std::string toSign(kChunkAlgorithm);
    toSign += '\n';
    toSign += amzDate;
    toSign += '\n';
    toSign += scope;
    toSign += '\n';
    toSign += previousSignature;
    toSign += '\n';
    toSign += kEmptySha256;  // the chunk carries no headers of its own
    toSign += '\n';
    toSign += chunkSha256Hex;
    return sign(signingKey, toSign);
}

std::string trailerSignature(std::string_view signingKey, std::string_view amzDate,
                             std::string_view scope, std::string_view previousSignature,
                             std::string_view trailerSha256Hex) {
    std::string toSign("AWS4-HMAC-SHA256-TRAILER");
    toSign += '\n';
    toSign += amzDate;
    toSign += '\n';
    toSign += scope;
    toSign += '\n';
    toSign += previousSignature;
    toSign += '\n';
    toSign += trailerSha256Hex;
    return sign(signingKey, toSign);
}

}  // namespace monobucket::s3
