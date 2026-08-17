#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "storage/records.hpp"

// Cross-origin resource sharing, to S3's rules.
//
// CORS is enforced by the browser, not by us — a rule that refuses an origin
// stops a script reading the response, and stops nothing else. So the value of
// getting this right is not access control; it is that a bucket which *should*
// be readable from a web app actually is, and that the failure when it is not
// says which of origin, method or header was the one that did not match.
//
// The matching is expressed over plain strings for the same reason SigV4 is:
// every rule in here is testable without a socket.

namespace monobucket::s3 {

/// What the request asked for, extracted from the headers a browser sends.
struct CorsQuery {
    std::string origin;

    /// For a preflight this is `Access-Control-Request-Method`; for a real
    /// request it is the method itself.
    std::string method;

    /// Lowercased names from `Access-Control-Request-Headers`. Empty on a real
    /// request — only a preflight asks about headers in advance.
    std::vector<std::string> requestedHeaders;
};

/// The headers to send back, or nothing when no rule matched.
struct CorsDecision {
    const CorsRule* rule = nullptr;

    /// The subset of `requestedHeaders` the matched rule permits, which is what
    /// `Access-Control-Allow-Headers` must echo.
    std::vector<std::string> allowedHeaders;

    explicit operator bool() const noexcept { return rule != nullptr; }
};

/// True when `pattern` matches `value`. S3 allows a single `*` anywhere in an
/// AllowedOrigin or AllowedHeader, standing for any run of characters.
bool corsPatternMatches(std::string_view pattern, std::string_view value);

/// The first rule that permits this request, in document order — S3 evaluates
/// rules top to bottom and the first match wins, so rule order is meaningful
/// and must be preserved through storage.
CorsDecision matchCors(const std::vector<CorsRule>& rules, const CorsQuery& query);

/// Normalises rules — methods uppercased, header names lowercased — and refuses
/// anything S3's schema does not allow. Throws S3Exception(MalformedXML).
///
/// Separate from the XML so the console, which sends JSON, is checked by the
/// same code as the S3 API rather than by a second implementation that would
/// drift. A silently dropped rule is worse than a refused document, because the
/// symptom appears later and somewhere else.
std::vector<CorsRule> validateCorsRules(std::vector<CorsRule> rules);

/// Reads a CORSConfiguration document, validated as above.
std::vector<CorsRule> parseCorsConfiguration(std::string_view document);

/// Renders rules back to a CORSConfiguration document.
std::string renderCorsConfiguration(const std::vector<CorsRule>& rules);

/// Splits and lowercases `Access-Control-Request-Headers`.
std::vector<std::string> splitRequestedHeaders(std::string_view value);

}  // namespace monobucket::s3
