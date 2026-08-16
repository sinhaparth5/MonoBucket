#include "s3/uri.hpp"

#include <algorithm>
#include <cctype>

namespace monobucket::s3 {
namespace {

constexpr char kHexDigits[] = "0123456789ABCDEF";

bool isUnreserved(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

int hexValue(unsigned char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string uriEncode(std::string_view in, bool encodeSlash) {
    std::string out;
    out.reserve(in.size());

    for (const char ch : in) {
        const auto c = static_cast<unsigned char>(ch);
        if (isUnreserved(c) || (c == '/' && !encodeSlash)) {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(kHexDigits[c >> 4]);
            out.push_back(kHexDigits[c & 0x0F]);
        }
    }
    return out;
}

std::string uriDecode(std::string_view in) {
    std::string out;
    out.reserve(in.size());

    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%' || i + 2 >= in.size()) {
            out.push_back(in[i]);
            continue;
        }

        const int hi = hexValue(static_cast<unsigned char>(in[i + 1]));
        const int lo = hexValue(static_cast<unsigned char>(in[i + 2]));
        if (hi < 0 || lo < 0) {
            // Not an escape after all. A key may legitimately contain '%'.
            out.push_back(in[i]);
            continue;
        }

        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return out;
}

std::vector<QueryParam> parseQuery(std::string_view rawQuery) {
    std::vector<QueryParam> params;
    if (rawQuery.empty()) return params;

    // A leading '?' is not part of the query string, but callers vary in
    // whether they strip it.
    if (rawQuery.front() == '?') rawQuery.remove_prefix(1);

    std::size_t pos = 0;
    while (pos <= rawQuery.size()) {
        const std::size_t amp = rawQuery.find('&', pos);
        const std::size_t end = (amp == std::string_view::npos) ? rawQuery.size() : amp;
        const std::string_view field = rawQuery.substr(pos, end - pos);

        if (!field.empty()) {
            QueryParam param;
            const std::size_t eq = field.find('=');
            if (eq == std::string_view::npos) {
                param.name = uriDecode(field);
            } else {
                param.name     = uriDecode(field.substr(0, eq));
                param.value    = uriDecode(field.substr(eq + 1));
                param.hasValue = true;
            }
            params.push_back(std::move(param));
        }

        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }

    return params;
}

std::string canonicalQueryString(const std::vector<QueryParam>& params, std::string_view omit) {
    struct Encoded {
        std::string name;
        std::string value;
    };

    std::vector<Encoded> encoded;
    encoded.reserve(params.size());

    for (const auto& param : params) {
        if (!omit.empty() && param.name == omit) continue;
        encoded.push_back({uriEncode(param.name, true), uriEncode(param.value, true)});
    }

    // Sorted by encoded name, then by encoded value — AWS sorts the encoded
    // forms, not the decoded ones, and the two orders differ once a name
    // contains a character that encodes above 'z'.
    std::sort(encoded.begin(), encoded.end(), [](const Encoded& a, const Encoded& b) {
        if (a.name != b.name) return a.name < b.name;
        return a.value < b.value;
    });

    std::string out;
    for (const auto& item : encoded) {
        if (!out.empty()) out.push_back('&');
        out += item.name;
        out.push_back('=');
        out += item.value;
    }
    return out;
}

std::optional<std::string> findQuery(const std::vector<QueryParam>& params,
                                     std::string_view name) {
    for (const auto& param : params) {
        if (param.name == name) return param.value;
    }
    return std::nullopt;
}

bool hasQuery(const std::vector<QueryParam>& params, std::string_view name) {
    return std::any_of(params.begin(), params.end(),
                       [name](const QueryParam& p) { return p.name == name; });
}

}  // namespace monobucket::s3
