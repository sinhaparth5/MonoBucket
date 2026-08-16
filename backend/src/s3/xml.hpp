#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The XML MonoBucket speaks on the wire.
//
// A general XML library is not wanted here. S3 uses a fixed, shallow document
// shape, and the two documents clients *send* us (CompleteMultipartUpload and
// DeleteObjects) arrive from the network — so the parser is written to be
// bounded and boring rather than complete. Entities beyond the five predefined
// ones, DOCTYPEs and processing instructions are refused, which also disposes
// of entity-expansion attacks without a special case for them.

namespace monobucket::s3 {

inline constexpr const char* kS3Namespace = "http://s3.amazonaws.com/doc/2006-03-01/";

/// Builds a response document. Elements are written in call order; the caller
/// is responsible for emitting them in the order the schema specifies, because
/// some clients parse positionally.
class XmlWriter {
public:
    /// Writes the declaration and opens `root`. An empty `xmlns` omits the
    /// attribute, which is what the error document wants.
    explicit XmlWriter(std::string_view root, std::string_view xmlns = kS3Namespace);

    void element(std::string_view name, std::string_view text);
    void element(std::string_view name, std::uint64_t value);

    /// Deliberately not an `element` overload. A `const char*` argument
    /// converts to `bool` by a standard conversion and to `std::string_view`
    /// only by a user-defined one, so the boolean overload would win for every
    /// string literal and quietly write `true` in place of the text.
    void booleanElement(std::string_view name, bool value);

    /// Writes `name` only when `text` is non-empty. S3 omits optional elements
    /// rather than sending them empty, and some clients tell the two apart.
    void elementIfSet(std::string_view name, std::string_view text);

    void open(std::string_view name);
    void close();

    /// Closes any open elements plus the root and returns the document.
    std::string finish();

private:
    void indent();

    std::string              out_;
    std::vector<std::string> stack_;
    bool                     finished_ = false;
};

/// Escapes text for an element body. Also escapes the characters that only
/// matter inside attributes, so one function covers both positions.
std::string xmlEscape(std::string_view text);

/// One element of a parsed document. Attributes are discarded: no request
/// document S3 defines carries one that changes its meaning.
struct XmlNode {
    std::string                  name;
    std::string                  text;
    std::vector<XmlNode>         children;

    /// First child with this name, or nullptr.
    const XmlNode* child(std::string_view childName) const;

    /// Text of the first child with this name, or an empty string.
    std::string childText(std::string_view childName) const;

    /// Every child with this name, in document order.
    std::vector<const XmlNode*> childrenNamed(std::string_view childName) const;
};

class XmlParseError : public std::runtime_error {
public:
    explicit XmlParseError(const std::string& what) : std::runtime_error(what) {}
};

/// Limits applied to any document arriving from a client. They are generous
/// against real requests and mean an adversarial one costs bounded work.
inline constexpr std::size_t kMaxXmlBytes    = 8ull * 1024 * 1024;
inline constexpr std::size_t kMaxXmlDepth    = 16;
inline constexpr std::size_t kMaxXmlElements = 200000;

/// Parses a document down to its root element. Throws XmlParseError on
/// anything malformed, unsupported or oversized — never on a well-formed
/// document whose elements we do not recognise, which is the caller's business.
XmlNode parseXml(std::string_view document);

}  // namespace monobucket::s3
