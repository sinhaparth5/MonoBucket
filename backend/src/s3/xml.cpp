#include "s3/xml.hpp"

#include <algorithm>
#include <cctype>

namespace monobucket::s3 {
namespace {

bool isNameStart(unsigned char c) noexcept {
    return std::isalpha(c) != 0 || c == '_' || c == ':';
}

bool isNameChar(unsigned char c) noexcept {
    return std::isalnum(c) != 0 || c == '_' || c == ':' || c == '-' || c == '.';
}

bool isSpace(unsigned char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/// Recursive-descent parser over a bounded document.
///
/// The recursion is bounded by kMaxXmlDepth rather than by the input, which is
/// what keeps a deeply nested document from being a stack overflow instead of
/// an error response.
class Parser {
public:
    explicit Parser(std::string_view input) : in_(input) {}

    XmlNode parseDocument() {
        skipProlog();
        XmlNode root = parseElement(0);
        skipMisc();
        if (pos_ != in_.size()) {
            throw XmlParseError("trailing content after the root element");
        }
        return root;
    }

private:
    [[noreturn]] void fail(const char* what) const { throw XmlParseError(what); }

    bool startsWith(std::string_view literal) const {
        return in_.compare(pos_, literal.size(), literal) == 0;
    }

    void skipSpace() {
        while (pos_ < in_.size() && isSpace(static_cast<unsigned char>(in_[pos_]))) ++pos_;
    }

    /// Comments and whitespace, which may appear before and after the root.
    void skipMisc() {
        for (;;) {
            skipSpace();
            if (!startsWith("<!--")) return;
            const std::size_t end = in_.find("-->", pos_ + 4);
            if (end == std::string_view::npos) fail("unterminated comment");
            pos_ = end + 3;
        }
    }

    void skipProlog() {
        skipSpace();
        if (startsWith("<?xml")) {
            const std::size_t end = in_.find("?>", pos_);
            if (end == std::string_view::npos) fail("unterminated XML declaration");
            pos_ = end + 2;
        }
        skipMisc();

        // A DOCTYPE is where external and internal entities would be declared.
        // Refusing it outright is both simpler and safer than parsing it and
        // then declining to expand what it defines.
        if (startsWith("<!DOCTYPE")) fail("DOCTYPE declarations are not accepted");
    }

    std::string parseName() {
        if (pos_ >= in_.size() || !isNameStart(static_cast<unsigned char>(in_[pos_]))) {
            fail("expected an element name");
        }
        const std::size_t start = pos_;
        while (pos_ < in_.size() && isNameChar(static_cast<unsigned char>(in_[pos_]))) ++pos_;
        return std::string(in_.substr(start, pos_ - start));
    }

    /// Attributes are consumed and discarded. Returns true for a self-closing
    /// tag, having consumed the closing delimiter either way.
    bool skipAttributes() {
        for (;;) {
            skipSpace();
            if (pos_ >= in_.size()) fail("unterminated start tag");

            if (in_[pos_] == '>') {
                ++pos_;
                return false;
            }
            if (startsWith("/>")) {
                pos_ += 2;
                return true;
            }

            (void)parseName();
            skipSpace();
            if (pos_ >= in_.size() || in_[pos_] != '=') fail("malformed attribute");
            ++pos_;
            skipSpace();
            if (pos_ >= in_.size() || (in_[pos_] != '"' && in_[pos_] != '\'')) {
                fail("unquoted attribute value");
            }
            const char quote = in_[pos_++];
            const std::size_t end = in_.find(quote, pos_);
            if (end == std::string_view::npos) fail("unterminated attribute value");
            pos_ = end + 1;
        }
    }

    /// Only the five predefined entities plus numeric character references.
    /// Anything else is a declaration we refused to read, so it cannot be
    /// resolved and must not be passed through silently.
    void appendEntity(std::string& out) {
        const std::size_t end = in_.find(';', pos_);
        if (end == std::string_view::npos || end - pos_ > 12) fail("unterminated entity reference");

        const std::string_view name = in_.substr(pos_ + 1, end - pos_ - 1);
        pos_ = end + 1;

        if (name == "lt")        { out.push_back('<');  return; }
        if (name == "gt")        { out.push_back('>');  return; }
        if (name == "amp")       { out.push_back('&');  return; }
        if (name == "quot")      { out.push_back('"');  return; }
        if (name == "apos")      { out.push_back('\''); return; }

        if (!name.empty() && name.front() == '#') {
            unsigned long code = 0;
            const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
            const std::string_view digits = name.substr(hex ? 2 : 1);
            if (digits.empty()) fail("malformed character reference");
            for (const char ch : digits) {
                const int value = hex ? (std::isdigit(static_cast<unsigned char>(ch))
                                             ? ch - '0'
                                             : (std::isxdigit(static_cast<unsigned char>(ch))
                                                    ? std::tolower(ch) - 'a' + 10
                                                    : -1))
                                      : (std::isdigit(static_cast<unsigned char>(ch)) ? ch - '0'
                                                                                     : -1);
                if (value < 0) fail("malformed character reference");
                code = code * (hex ? 16 : 10) + static_cast<unsigned long>(value);
                if (code > 0x10FFFF) fail("character reference out of range");
            }
            appendUtf8(out, static_cast<std::uint32_t>(code));
            return;
        }

        fail("unknown entity reference");
    }

    static void appendUtf8(std::string& out, std::uint32_t code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    XmlNode parseElement(std::size_t depth) {
        if (depth >= kMaxXmlDepth) fail("document nested too deeply");
        if (++elements_ > kMaxXmlElements) fail("document has too many elements");

        if (pos_ >= in_.size() || in_[pos_] != '<') fail("expected an element");
        ++pos_;

        XmlNode node;
        node.name = parseName();
        if (skipAttributes()) return node;  // self-closing

        for (;;) {
            if (pos_ >= in_.size()) fail("unterminated element");

            if (in_[pos_] == '<') {
                if (startsWith("</")) {
                    pos_ += 2;
                    const std::string closing = parseName();
                    if (closing != node.name) fail("mismatched closing tag");
                    skipSpace();
                    if (pos_ >= in_.size() || in_[pos_] != '>') fail("malformed closing tag");
                    ++pos_;
                    // Text is only meaningful on a leaf. Whitespace between
                    // child elements is formatting, not content.
                    if (!node.children.empty()) node.text.clear();
                    return node;
                }
                if (startsWith("<!--")) {
                    const std::size_t end = in_.find("-->", pos_ + 4);
                    if (end == std::string_view::npos) fail("unterminated comment");
                    pos_ = end + 3;
                    continue;
                }
                if (startsWith("<![CDATA[")) {
                    const std::size_t end = in_.find("]]>", pos_ + 9);
                    if (end == std::string_view::npos) fail("unterminated CDATA section");
                    node.text.append(in_.substr(pos_ + 9, end - pos_ - 9));
                    pos_ = end + 3;
                    continue;
                }
                if (startsWith("<?")) fail("processing instructions are not accepted");
                node.children.push_back(parseElement(depth + 1));
                continue;
            }

            if (in_[pos_] == '&') {
                appendEntity(node.text);
                continue;
            }

            node.text.push_back(in_[pos_++]);
        }
    }

    std::string_view in_;
    std::size_t      pos_      = 0;
    std::size_t      elements_ = 0;
};

/// Trims the whitespace an XML formatter introduces around element text. S3
/// values that could legitimately carry leading or trailing spaces — an object
/// key in a DeleteObjects document — never appear as bare text; they arrive
/// escaped or not at all.
std::string_view trimmed(std::string_view text) {
    while (!text.empty() && isSpace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace

// --- Writer ----------------------------------------------------------------

XmlWriter::XmlWriter(std::string_view root, std::string_view xmlns) {
    out_ = R"(<?xml version="1.0" encoding="UTF-8"?>)";
    out_ += '\n';
    out_ += '<';
    out_ += root;
    if (!xmlns.empty()) {
        out_ += R"( xmlns=")";
        out_ += xmlns;
        out_ += '"';
    }
    out_ += ">\n";
    stack_.emplace_back(root);
}

void XmlWriter::indent() {
    out_.append(stack_.size() * 2, ' ');
}

void XmlWriter::element(std::string_view name, std::string_view text) {
    indent();
    out_ += '<';
    out_ += name;
    out_ += '>';
    out_ += xmlEscape(text);
    out_ += "</";
    out_ += name;
    out_ += ">\n";
}

void XmlWriter::element(std::string_view name, std::uint64_t value) {
    element(name, std::to_string(value));
}

void XmlWriter::booleanElement(std::string_view name, bool value) {
    element(name, value ? std::string_view("true") : std::string_view("false"));
}

void XmlWriter::elementIfSet(std::string_view name, std::string_view text) {
    if (!text.empty()) element(name, text);
}

void XmlWriter::open(std::string_view name) {
    indent();
    out_ += '<';
    out_ += name;
    out_ += ">\n";
    stack_.emplace_back(name);
}

void XmlWriter::close() {
    if (stack_.size() <= 1) return;  // the root is closed by finish()
    const std::string name = stack_.back();
    stack_.pop_back();
    indent();
    out_ += "</";
    out_ += name;
    out_ += ">\n";
}

std::string XmlWriter::finish() {
    if (!finished_) {
        while (stack_.size() > 1) close();
        if (!stack_.empty()) {
            out_ += "</";
            out_ += stack_.back();
            out_ += ">\n";
            stack_.clear();
        }
        finished_ = true;
    }
    return out_;
}

std::string xmlEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size());

    for (const char ch : text) {
        switch (ch) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                // XML 1.0 cannot represent most control characters at all, not
                // even escaped. An object key may contain them, which is what
                // `encoding-type=url` exists for; dropping them here keeps the
                // document parseable for a client that did not ask for it.
                if (static_cast<unsigned char>(ch) >= 0x20 || ch == '\t' || ch == '\n' ||
                    ch == '\r') {
                    out.push_back(ch);
                }
                break;
        }
    }
    return out;
}

// --- Reader ----------------------------------------------------------------

const XmlNode* XmlNode::child(std::string_view childName) const {
    for (const auto& node : children) {
        if (node.name == childName) return &node;
    }
    return nullptr;
}

std::string XmlNode::childText(std::string_view childName) const {
    const XmlNode* node = child(childName);
    return node == nullptr ? std::string() : std::string(trimmed(node->text));
}

std::vector<const XmlNode*> XmlNode::childrenNamed(std::string_view childName) const {
    std::vector<const XmlNode*> matches;
    for (const auto& node : children) {
        if (node.name == childName) matches.push_back(&node);
    }
    return matches;
}

XmlNode parseXml(std::string_view document) {
    if (document.size() > kMaxXmlBytes) throw XmlParseError("document is too large");
    if (trimmed(document).empty()) throw XmlParseError("document is empty");
    return Parser(document).parseDocument();
}

}  // namespace monobucket::s3
