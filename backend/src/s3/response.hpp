#pragma once

#include <string>
#include <string_view>

#include <drogon/HttpResponse.h>

#include "s3/s3_error.hpp"

namespace monobucket::s3 {

/// Headers every S3 response carries. The two request ids are what AWS support
/// asks for, and clients log them verbatim, so a bug report against MonoBucket
/// can be traced to a line in our own log by the same string.
void applyCommonHeaders(const drogon::HttpResponsePtr& response, std::string_view requestId);

drogon::HttpResponsePtr xmlResponse(std::string body, int status = 200);

/// A response with no body — PUT, DELETE and the 304 path.
drogon::HttpResponsePtr emptyResponse(int status);

drogon::HttpResponsePtr errorResponse(S3ErrorCode code, std::string_view message,
                                      std::string_view resource, std::string_view requestId);

/// Drogon's status enum from a plain integer. Codes it does not know become
/// k500InternalServerError rather than a malformed status line.
drogon::HttpStatusCode toStatus(int status) noexcept;

}  // namespace monobucket::s3
