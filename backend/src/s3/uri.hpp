#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Percent-encoding and query-string handling, to the rules SigV4 uses.
//
// These are deliberately *not* the generic URL helpers Drogon already ships.
// Signature verification compares byte strings, so the only encoder that can be
// used here is the one AWS specifies, differences from RFC 3986 included.

namespace monobucket::s3 {

/// Percent-encodes to the AWS unreserved set: `A-Za-z0-9-_.~` pass through,
/// everything else becomes `%XX` with uppercase hex.
///
/// `encodeSlash` is false for a URI path — an object key's separators must
/// survive — and true everywhere else, notably for query parameters.
std::string uriEncode(std::string_view in, bool encodeSlash);

/// Reverses `%XX`. A `+` is returned as itself, not as a space: SigV4
/// canonicalisation decodes and re-encodes every query parameter, and treating
/// `+` as a space there would rewrite a signed byte string into a different one
/// and reject a valid request. S3 does not accept `+` for space either, so this
/// is faithful in both directions.
///
/// A malformed escape (`%` not followed by two hex digits) is kept verbatim
/// rather than rejected; a key is allowed to contain a literal percent sign.
std::string uriDecode(std::string_view in);

struct QueryParam {
    std::string name;
    std::string value;

    /// Distinguishes `?acl` from `?acl=`. S3 uses bare flags for subresources,
    /// and the canonical query string renders the two identically, but handler
    /// code occasionally needs to tell them apart.
    bool hasValue = false;
};

/// Splits a raw query string, decoding names and values. Order is preserved as
/// sent; canonicalQueryString() does the sorting.
std::vector<QueryParam> parseQuery(std::string_view rawQuery);

/// The CanonicalQueryString of a canonical request: every name and value
/// re-encoded, sorted by name and then by value, joined with `&`, and a bare
/// flag rendered as `name=`.
///
/// `omit` drops one parameter by name, which is how the presigned-URL flow
/// excludes `X-Amz-Signature` from the string it is verifying.
std::string canonicalQueryString(const std::vector<QueryParam>& params,
                                 std::string_view omit = {});

/// First value for `name`, or nullopt when absent. Comparison is
/// case-sensitive, as S3 query parameters are.
std::optional<std::string> findQuery(const std::vector<QueryParam>& params,
                                     std::string_view name);

/// True when the parameter is present in any form, valued or bare.
bool hasQuery(const std::vector<QueryParam>& params, std::string_view name);

}  // namespace monobucket::s3
