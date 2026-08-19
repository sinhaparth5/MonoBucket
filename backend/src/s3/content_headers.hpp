#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "s3/request.hpp"
#include "storage/records.hpp"

// Deciding what an object read carries in Cache-Control, Content-Disposition,
// Content-Encoding, Content-Language and Expires.
//
// Separate from handlers.hpp, which pulls in Drogon, for the reason SigV4 is
// expressed over plain strings: the precedence rule below is what every
// presigned download link depends on, and it should be testable without a
// socket. Reading the headers off an incoming request is the other half and
// necessarily does know about Drogon — it lives in handlers.hpp.

namespace monobucket::s3 {

/// One header a read should carry, and the value to carry it with.
struct ResolvedHeader {
    std::string_view name;
    std::string      value;
};

/// The content headers a GET or HEAD should emit: what was stored with the
/// object, except where the request's `response-*` query parameter overrides
/// it. A header with neither is omitted rather than sent empty, and an override
/// applies even to an object that stored nothing — a link that asks for an
/// attachment gets one either way.
///
/// Content-Type is not among them. It has a field of its own on the record, a
/// default of its own, and it reaches the response through
/// `setContentTypeString` rather than as a header.
std::vector<ResolvedHeader> resolveContentHeaders(const ContentHeaders& stored,
                                                  const S3Request&      request);

}  // namespace monobucket::s3
