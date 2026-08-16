#include "s3/response.hpp"

#include <drogon/HttpTypes.h>

#include "monobucket/constants.hpp"

namespace monobucket::s3 {

void applyCommonHeaders(const drogon::HttpResponsePtr& response, std::string_view requestId) {
    response->addHeader(headers::kAmzRequestId, std::string(requestId));
    // AWS's extended id. Clients only ever echo it back, so it carries the same
    // value rather than a second identifier nothing would correlate with.
    response->addHeader("x-amz-id-2", std::string(requestId));
}

drogon::HttpResponsePtr xmlResponse(std::string body, int status) {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(toStatus(status));
    response->setContentTypeString("application/xml");
    response->setBody(std::move(body));
    return response;
}

drogon::HttpResponsePtr emptyResponse(int status) {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(toStatus(status));
    // Drogon defaults to text/html, which would be a lie on an empty body and
    // confuses at least one client into trying to parse it.
    response->setContentTypeCode(drogon::CT_NONE);
    response->setBody("");
    return response;
}

drogon::HttpResponsePtr errorResponse(S3ErrorCode code, std::string_view message,
                                      std::string_view resource, std::string_view requestId) {
    const S3ErrorInfo& info = describe(code);
    auto response = xmlResponse(renderError(code, message, resource, requestId), info.status);
    applyCommonHeaders(response, requestId);
    return response;
}

drogon::HttpStatusCode toStatus(int status) noexcept {
    switch (status) {
        case 200: return drogon::k200OK;
        case 204: return drogon::k204NoContent;
        case 206: return drogon::k206PartialContent;
        case 304: return drogon::k304NotModified;
        case 400: return drogon::k400BadRequest;
        case 403: return drogon::k403Forbidden;
        case 404: return drogon::k404NotFound;
        case 405: return drogon::k405MethodNotAllowed;
        case 409: return drogon::k409Conflict;
        case 411: return drogon::k411LengthRequired;
        case 412: return drogon::k412PreconditionFailed;
        case 416: return drogon::k416RequestedRangeNotSatisfiable;
        case 500: return drogon::k500InternalServerError;
        case 501: return drogon::k501NotImplemented;
        case 503: return drogon::k503ServiceUnavailable;
        default:  return drogon::k500InternalServerError;
    }
}

}  // namespace monobucket::s3
