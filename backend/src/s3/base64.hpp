#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Standard base64, as used by Content-MD5 and by the continuation tokens in
// ListObjectsV2.
//
// OpenSSL's BIO-based encoder is available and is not worth the four objects it
// takes to configure correctly, nor its habit of inserting newlines by default.

namespace monobucket::s3 {

std::string base64Encode(std::string_view data);

/// Nullopt for anything that is not well-formed base64, padding included.
/// Strict on purpose: a continuation token is opaque to the client, so a
/// malformed one means either corruption or someone probing.
std::optional<std::string> base64Decode(std::string_view text);

}  // namespace monobucket::s3
