#include "s3/content_headers.hpp"

#include <iterator>
#include <utility>

namespace monobucket::s3 {

std::vector<ResolvedHeader> resolveContentHeaders(const ContentHeaders& stored,
                                                  const S3Request&      request) {
    struct Entry {
        std::string_view   name;
        const char*        parameter;
        const std::string& value;
    };

    // Emitted in this order rather than in whatever order they happen to be
    // written, so two reads of the same object produce the same response.
    const Entry entries[] = {
        {"Cache-Control",       "response-cache-control",       stored.cacheControl},
        {"Content-Disposition", "response-content-disposition", stored.contentDisposition},
        {"Content-Encoding",    "response-content-encoding",    stored.contentEncoding},
        {"Content-Language",    "response-content-language",    stored.contentLanguage},
        {"Expires",             "response-expires",             stored.expires},
    };

    std::vector<ResolvedHeader> resolved;
    resolved.reserve(std::size(entries));

    for (const Entry& entry : entries) {
        // A parameter that is present but blank is not an override: it says
        // nothing, and the stored value stands. S3 offers no way to clear a
        // stored header for one response, and inventing one here would make a
        // truncated link silently change what the object is.
        const auto  override_ = request.queryValue(entry.parameter);
        std::string value = (override_ && !override_->empty()) ? *override_ : entry.value;
        if (value.empty()) continue;

        resolved.push_back({entry.name, std::move(value)});
    }
    return resolved;
}

}  // namespace monobucket::s3
