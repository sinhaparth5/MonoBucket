#include "s3/cors.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

#include "s3/handlers.hpp"
#include "s3/response.hpp"
#include "s3/s3_error.hpp"
#include "s3/xml.hpp"
#include "storage/storage_engine.hpp"

namespace monobucket::s3 {
namespace {

/// S3's ceiling. A configuration is walked linearly on every cross-origin
/// request, so the bound is what keeps that walk a constant rather than
/// something a client controls.
constexpr std::size_t kMaxRules = 100;

/// The methods a CORS rule may name. This is not the set of methods the server
/// implements — it is the set S3 accepts in a configuration document, and a
/// rule naming anything else is refused rather than ignored.
constexpr std::array<std::string_view, 5> kAllowedMethods{"GET", "PUT", "POST", "DELETE", "HEAD"};

std::string toLower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string toUpper(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

std::string trim(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(begin, end - begin + 1));
}

std::string join(const std::vector<std::string>& values, std::string_view separator) {
    std::string out;
    for (const std::string& value : values) {
        if (!out.empty()) out += separator;
        out += value;
    }
    return out;
}

bool matchesAny(const std::vector<std::string>& patterns, std::string_view value) {
    return std::any_of(patterns.begin(), patterns.end(),
                       [value](const std::string& pattern) {
                           return corsPatternMatches(pattern, value);
                       });
}

/// Refuses the patterns S3 refuses, so a document that would behave
/// unpredictably is rejected at the point it can still be corrected.
void validatePattern(std::string_view pattern, const char* what) {
    if (pattern.empty()) {
        throw S3Exception(S3ErrorCode::MalformedXML,
                          std::string(what) + " must not be empty.");
    }
    if (std::count(pattern.begin(), pattern.end(), '*') > 1) {
        throw S3Exception(S3ErrorCode::MalformedXML,
                          std::string(what) + " may contain at most one wildcard.");
    }
}

}  // namespace

bool corsPatternMatches(std::string_view pattern, std::string_view value) {
    const std::size_t star = pattern.find('*');
    if (star == std::string_view::npos) return pattern == value;

    const std::string_view prefix = pattern.substr(0, star);
    const std::string_view suffix = pattern.substr(star + 1);

    // The halves must not overlap, or `ab*ba` would match `aba` twice over.
    if (value.size() < prefix.size() + suffix.size()) return false;

    return value.compare(0, prefix.size(), prefix) == 0 &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> splitRequestedHeaders(std::string_view value) {
    std::vector<std::string> names;
    std::size_t              pos = 0;

    while (pos <= value.size()) {
        const std::size_t comma = value.find(',', pos);
        const std::size_t end   = (comma == std::string_view::npos) ? value.size() : comma;

        std::string name = trim(value.substr(pos, end - pos));
        if (!name.empty()) names.push_back(toLower(name));

        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return names;
}

CorsDecision matchCors(const std::vector<CorsRule>& rules, const CorsQuery& query) {
    for (const CorsRule& rule : rules) {
        if (!matchesAny(rule.allowedOrigins, query.origin)) continue;

        const std::string method = toUpper(query.method);
        if (std::find(rule.allowedMethods.begin(), rule.allowedMethods.end(), method) ==
            rule.allowedMethods.end()) {
            continue;
        }

        // A rule that permits the origin and method but not every header the
        // preflight asked about does not match at all — the next rule gets a
        // turn. Answering with a partial header list instead would let the
        // browser send a request this configuration never allowed.
        const bool headersOk =
            std::all_of(query.requestedHeaders.begin(), query.requestedHeaders.end(),
                        [&rule](const std::string& name) {
                            return matchesAny(rule.allowedHeaders, name);
                        });
        if (!headersOk) continue;

        CorsDecision decision;
        decision.rule           = &rule;
        decision.allowedHeaders = query.requestedHeaders;
        return decision;
    }
    return {};
}

std::vector<CorsRule> validateCorsRules(std::vector<CorsRule> rules) {
    if (rules.empty()) {
        throw S3Exception(S3ErrorCode::MalformedXML,
                          "A CORS configuration must contain at least one rule.");
    }
    if (rules.size() > kMaxRules) {
        throw S3Exception(S3ErrorCode::MalformedXML,
                          "A CORS configuration may contain at most " + std::to_string(kMaxRules) +
                              " rules.");
    }

    for (CorsRule& rule : rules) {
        if (rule.allowedOrigins.empty()) {
            throw S3Exception(S3ErrorCode::MalformedXML,
                              "Every rule needs at least one allowed origin.");
        }
        for (std::string& origin : rule.allowedOrigins) validatePattern(origin, "AllowedOrigin");

        if (rule.allowedMethods.empty()) {
            throw S3Exception(S3ErrorCode::MalformedXML,
                              "Every rule needs at least one allowed method.");
        }
        for (std::string& method : rule.allowedMethods) {
            method = toUpper(method);
            if (std::find(kAllowedMethods.begin(), kAllowedMethods.end(), method) ==
                kAllowedMethods.end()) {
                throw S3Exception(S3ErrorCode::MalformedXML,
                                  "'" + method +
                                      "' is not an allowed method. Use GET, PUT, POST, DELETE or "
                                      "HEAD.");
            }
        }

        for (std::string& header : rule.allowedHeaders) {
            validatePattern(header, "AllowedHeader");
            header = toLower(header);
        }
        for (const std::string& header : rule.exposeHeaders) {
            if (header.empty()) {
                throw S3Exception(S3ErrorCode::MalformedXML,
                                  "An exposed header name must not be empty.");
            }
        }

        if (rule.maxAgeSeconds > 86400) {
            throw S3Exception(S3ErrorCode::MalformedXML,
                              "MaxAgeSeconds must be between 0 and 86400.");
        }
    }

    return rules;
}

std::vector<CorsRule> parseCorsConfiguration(std::string_view document) {
    if (document.empty()) {
        throw S3Exception(S3ErrorCode::MalformedXML, "The CORS configuration is empty.");
    }

    XmlNode root;
    try {
        root = parseXml(document);
    } catch (const XmlParseError& error) {
        throw S3Exception(S3ErrorCode::MalformedXML, error.what());
    }

    if (root.name != "CORSConfiguration") {
        throw S3Exception(S3ErrorCode::MalformedXML,
                          "The root element must be <CORSConfiguration>.");
    }

    // Extraction only. Everything that decides whether the document is
    // acceptable lives in validateCorsRules, which the console reaches by a
    // different door.
    std::vector<CorsRule> rules;
    for (const XmlNode* node : root.childrenNamed("CORSRule")) {
        CorsRule rule;
        rule.id = node->childText("ID");

        for (const XmlNode* origin : node->childrenNamed("AllowedOrigin")) {
            rule.allowedOrigins.push_back(origin->text);
        }
        for (const XmlNode* method : node->childrenNamed("AllowedMethod")) {
            rule.allowedMethods.push_back(method->text);
        }
        for (const XmlNode* header : node->childrenNamed("AllowedHeader")) {
            rule.allowedHeaders.push_back(header->text);
        }
        for (const XmlNode* header : node->childrenNamed("ExposeHeader")) {
            rule.exposeHeaders.push_back(header->text);
        }

        if (const XmlNode* maxAge = node->child("MaxAgeSeconds")) {
            try {
                const long long value = std::stoll(maxAge->text);
                // Clamped into the field's range before validation rather than
                // truncated by the cast, so an absurd value is reported as out
                // of range instead of wrapping into an acceptable one.
                if (value < 0 || value > 86400) {
                    throw S3Exception(S3ErrorCode::MalformedXML,
                                      "MaxAgeSeconds must be between 0 and 86400.");
                }
                rule.maxAgeSeconds = static_cast<std::int32_t>(value);
            } catch (const std::invalid_argument&) {
                throw S3Exception(S3ErrorCode::MalformedXML, "MaxAgeSeconds must be a number.");
            } catch (const std::out_of_range&) {
                throw S3Exception(S3ErrorCode::MalformedXML, "MaxAgeSeconds is out of range.");
            }
        }

        rules.push_back(std::move(rule));
    }

    if (rules.empty()) {
        throw S3Exception(S3ErrorCode::MalformedXML,
                          "A CORS configuration must contain at least one <CORSRule>.");
    }
    return validateCorsRules(std::move(rules));
}

std::string renderCorsConfiguration(const std::vector<CorsRule>& rules) {
    XmlWriter writer("CORSConfiguration");

    for (const CorsRule& rule : rules) {
        writer.open("CORSRule");
        writer.elementIfSet("ID", rule.id);
        for (const std::string& value : rule.allowedOrigins) writer.element("AllowedOrigin", value);
        for (const std::string& value : rule.allowedMethods) writer.element("AllowedMethod", value);
        for (const std::string& value : rule.allowedHeaders) writer.element("AllowedHeader", value);
        for (const std::string& value : rule.exposeHeaders) writer.element("ExposeHeader", value);
        if (rule.maxAgeSeconds >= 0) {
            writer.element("MaxAgeSeconds", static_cast<std::uint64_t>(rule.maxAgeSeconds));
        }
        writer.close();
    }

    return writer.finish();
}

// --- Handlers --------------------------------------------------------------

drogon::HttpResponsePtr handleGetBucketCors(const S3Context& context, const S3Request& request) {
    const BucketRecord bucket = requireBucket(context, request.bucket);
    if (bucket.cors.empty()) throw S3Exception(S3ErrorCode::NoSuchCORSConfiguration);

    auto response = xmlResponse(renderCorsConfiguration(bucket.cors));
    applyCommonHeaders(response, request.requestId);
    return response;
}

drogon::HttpResponsePtr handlePutBucketCors(const S3Context& context, const S3Request& request,
                                            const S3Body& body) {
    requireBucket(context, request.bucket);

    std::vector<CorsRule> rules = parseCorsConfiguration(body.materialise());
    context.storage.setBucketCors(request.bucket, std::move(rules));
    invalidateBucket(context, request.bucket);

    auto response = emptyResponse(200);
    applyCommonHeaders(response, request.requestId);
    return response;
}

drogon::HttpResponsePtr handleDeleteBucketCors(const S3Context& context, const S3Request& request) {
    requireBucket(context, request.bucket);

    context.storage.setBucketCors(request.bucket, {});
    invalidateBucket(context, request.bucket);

    auto response = emptyResponse(204);
    applyCommonHeaders(response, request.requestId);
    return response;
}

drogon::HttpResponsePtr handlePreflight(const S3Context& context, const S3Request& request,
                                        const drogon::HttpRequestPtr& http) {
    const std::string origin = http->getHeader("origin");
    const std::string method = http->getHeader("access-control-request-method");

    // Not an error a browser can produce; this is somebody testing by hand, and
    // naming the missing header saves them the guess.
    if (origin.empty() || method.empty()) {
        throw S3Exception(S3ErrorCode::InvalidRequest,
                          "A preflight needs the Origin and Access-Control-Request-Method "
                          "headers.");
    }
    if (request.bucket.empty()) {
        throw S3Exception(S3ErrorCode::AccessForbidden,
                          "CORSResponse: The service root has no CORS configuration.");
    }

    std::vector<CorsRule> rules;
    try {
        rules = requireBucket(context, request.bucket).cors;
    } catch (const S3Exception&) {
        // Nothing here is authenticated, so a missing bucket and a bucket that
        // permits nothing must be indistinguishable: answering NoSuchBucket
        // would turn the preflight into an unauthenticated existence oracle.
        rules.clear();
    } catch (const StorageError&) {
        rules.clear();
    }
    if (rules.empty()) {
        throw S3Exception(S3ErrorCode::AccessForbidden,
                          "CORSResponse: CORS is not enabled for this bucket.");
    }

    CorsQuery query;
    query.origin           = origin;
    query.method           = method;
    query.requestedHeaders =
        splitRequestedHeaders(http->getHeader("access-control-request-headers"));

    const CorsDecision decision = matchCors(rules, query);
    if (!decision) {
        // Which of the three did not match is the entire question a developer
        // has when a preflight fails, and the browser will not show them the
        // body — but it goes to the server log and to anyone testing by hand,
        // and that is where the answer is looked for second.
        CorsQuery withoutHeaders = query;
        withoutHeaders.requestedHeaders.clear();

        const std::string reason =
            (!query.requestedHeaders.empty() && matchCors(rules, withoutHeaders))
                ? "No rule permits header '" + join(query.requestedHeaders, ", ") +
                      "' for origin '" + origin + "'."
                : "No rule permits origin '" + origin + "' with method " + method + ".";

        throw S3Exception(S3ErrorCode::AccessForbidden,
                          "CORSResponse: This CORS request is not allowed. " + reason);
    }

    auto response = emptyResponse(200);
    applyCommonHeaders(response, request.requestId);

    // The origin is echoed rather than answered with `*` even when the rule is
    // a wildcard: a browser refuses `*` together with credentials, and a
    // presigned or signed cross-origin fetch is exactly the case this exists
    // for. Vary keeps a shared cache from serving one origin's answer to
    // another.
    response->addHeader("Access-Control-Allow-Origin", origin);
    response->addHeader("Access-Control-Allow-Methods", join(decision.rule->allowedMethods, ", "));
    response->addHeader("Access-Control-Allow-Credentials", "true");
    response->addHeader("Vary",
                        "Origin, Access-Control-Request-Method, Access-Control-Request-Headers");

    if (!decision.allowedHeaders.empty()) {
        response->addHeader("Access-Control-Allow-Headers", join(decision.allowedHeaders, ", "));
    }
    if (!decision.rule->exposeHeaders.empty()) {
        response->addHeader("Access-Control-Expose-Headers",
                            join(decision.rule->exposeHeaders, ", "));
    }
    if (decision.rule->maxAgeSeconds >= 0) {
        response->addHeader("Access-Control-Max-Age", std::to_string(decision.rule->maxAgeSeconds));
    }

    return response;
}

void applyCorsHeaders(const S3Context& context, const S3Request& request,
                      const drogon::HttpRequestPtr& http,
                      const drogon::HttpResponsePtr& response) {
    const std::string origin = http->getHeader("origin");
    if (origin.empty()) return;

    // Set before the rules are even consulted: the answer to this request
    // depends on the Origin header whether or not a rule matched, and a cache
    // that does not know that will serve the wrong one.
    response->addHeader("Vary", "Origin");

    if (request.bucket.empty()) return;

    std::vector<CorsRule> rules;
    try {
        rules = requireBucket(context, request.bucket).cors;
    } catch (const S3Exception&) {
        // The bucket does not exist or the name is invalid. The response is
        // already an error saying so; there is nothing to permit.
        return;
    } catch (const StorageError&) {
        return;
    }
    if (rules.empty()) return;

    CorsQuery query;
    query.origin = origin;
    query.method = request.method;

    const CorsDecision decision = matchCors(rules, query);
    if (!decision) return;

    response->addHeader("Access-Control-Allow-Origin", origin);
    response->addHeader("Access-Control-Allow-Methods", join(decision.rule->allowedMethods, ", "));
    response->addHeader("Access-Control-Allow-Credentials", "true");

    // Without this a script can read the body but not the ETag, which is what
    // a browser uploader needs in order to assemble a multipart completion.
    if (!decision.rule->exposeHeaders.empty()) {
        response->addHeader("Access-Control-Expose-Headers",
                            join(decision.rule->exposeHeaders, ", "));
    }
}

}  // namespace monobucket::s3
