#include <catch2/catch_test_macros.hpp>

#include "s3/xml.hpp"

using namespace monobucket::s3;

TEST_CASE("the writer produces a namespaced document", "[s3][xml]") {
    XmlWriter writer("ListAllMyBucketsResult");
    writer.open("Buckets");
    writer.open("Bucket");
    writer.element("Name", "photos");
    writer.element("Size", static_cast<std::uint64_t>(42));
    writer.close();
    writer.close();

    const std::string document = writer.finish();
    REQUIRE(document.find(R"(<?xml version="1.0" encoding="UTF-8"?>)") == 0);
    REQUIRE(document.find(kS3Namespace) != std::string::npos);
    REQUIRE(document.find("<Name>photos</Name>") != std::string::npos);
    REQUIRE(document.find("<Size>42</Size>") != std::string::npos);
    REQUIRE(document.find("</ListAllMyBucketsResult>") != std::string::npos);
}

TEST_CASE("finish closes whatever is still open", "[s3][xml]") {
    XmlWriter writer("Result");
    writer.open("Outer");
    writer.open("Inner");
    writer.element("Key", "k");

    // A handler that returns early through an exception path must not produce a
    // document that cannot be parsed.
    const std::string document = writer.finish();
    REQUIRE(document.find("</Inner>") != std::string::npos);
    REQUIRE(document.find("</Outer>") != std::string::npos);
    REQUIRE(document.find("</Result>") != std::string::npos);
    REQUIRE_NOTHROW(parseXml(document));
}

TEST_CASE("element text is escaped", "[s3][xml]") {
    XmlWriter writer("Result");
    writer.element("Key", R"(a&b<c>d"e'f)");
    const std::string document = writer.finish();

    REQUIRE(document.find("&amp;") != std::string::npos);
    REQUIRE(document.find("&lt;") != std::string::npos);
    REQUIRE(document.find("&gt;") != std::string::npos);
    REQUIRE(document.find("&quot;") != std::string::npos);
    REQUIRE(document.find("&apos;") != std::string::npos);

    const XmlNode root = parseXml(document);
    REQUIRE(root.childText("Key") == R"(a&b<c>d"e'f)");
}

TEST_CASE("control characters are dropped rather than emitted", "[s3][xml]") {
    // XML 1.0 cannot represent them even escaped, so a key containing one must
    // not be able to produce a document the client cannot parse.
    XmlWriter writer("Result");
    writer.element("Key", std::string("bad\x01key", 7));

    const std::string document = writer.finish();
    REQUIRE(document.find('\x01') == std::string::npos);
    REQUIRE_NOTHROW(parseXml(document));
}

TEST_CASE("elementIfSet omits an empty value", "[s3][xml]") {
    XmlWriter writer("Result");
    writer.elementIfSet("Delimiter", "");
    writer.elementIfSet("Prefix", "photos/");

    const std::string document = writer.finish();
    REQUIRE(document.find("Delimiter") == std::string::npos);
    REQUIRE(document.find("<Prefix>photos/</Prefix>") != std::string::npos);
}

TEST_CASE("a CompleteMultipartUpload document parses", "[s3][xml]") {
    const XmlNode root = parseXml(R"(<?xml version="1.0" encoding="UTF-8"?>
        <CompleteMultipartUpload xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
          <Part><PartNumber>1</PartNumber><ETag>&quot;abc&quot;</ETag></Part>
          <Part><PartNumber>2</PartNumber><ETag>"def"</ETag></Part>
        </CompleteMultipartUpload>)");

    REQUIRE(root.name == "CompleteMultipartUpload");

    const auto parts = root.childrenNamed("Part");
    REQUIRE(parts.size() == 2);
    REQUIRE(parts[0]->childText("PartNumber") == "1");
    REQUIRE(parts[0]->childText("ETag") == "\"abc\"");
    REQUIRE(parts[1]->childText("PartNumber") == "2");
}

TEST_CASE("a DeleteObjects document parses", "[s3][xml]") {
    const XmlNode root = parseXml(
        "<Delete><Quiet>true</Quiet>"
        "<Object><Key>a/b.txt</Key></Object>"
        "<Object><Key>c.txt</Key><VersionId>null</VersionId></Object>"
        "</Delete>");

    REQUIRE(root.childText("Quiet") == "true");
    const auto objects = root.childrenNamed("Object");
    REQUIRE(objects.size() == 2);
    REQUIRE(objects[0]->childText("Key") == "a/b.txt");
    REQUIRE(objects[1]->childText("Key") == "c.txt");
}

TEST_CASE("whitespace between elements is not text", "[s3][xml]") {
    // An indented document must not turn a container element's formatting into
    // content, or every pretty-printed request would carry stray whitespace.
    const XmlNode root = parseXml("<Delete>\n  <Object>\n    <Key>k</Key>\n  </Object>\n</Delete>");
    REQUIRE(root.text.empty());
    REQUIRE(root.childText("Object").empty());
    REQUIRE(root.child("Object")->childText("Key") == "k");
}

TEST_CASE("self-closing elements, comments and CDATA are handled", "[s3][xml]") {
    const XmlNode root = parseXml(
        "<Delete><!-- comment --><Quiet/><Object><Key><![CDATA[a<b]]></Key></Object></Delete>");

    REQUIRE(root.child("Quiet") != nullptr);
    REQUIRE(root.child("Quiet")->text.empty());
    REQUIRE(root.child("Object")->childText("Key") == "a<b");
}

TEST_CASE("attributes are accepted and ignored", "[s3][xml]") {
    const XmlNode root = parseXml(R"(<Delete xmlns="urn:x" a='1' b="2"><Quiet>true</Quiet></Delete>)");
    REQUIRE(root.childText("Quiet") == "true");
}

TEST_CASE("numeric character references decode to UTF-8", "[s3][xml]") {
    REQUIRE(parseXml("<K>&#233;</K>").text == "\xc3\xa9");
    REQUIRE(parseXml("<K>&#xE9;</K>").text == "\xc3\xa9");
    REQUIRE(parseXml("<K>&#128169;</K>").text == "\xf0\x9f\x92\xa9");
}

TEST_CASE("hostile documents are refused rather than expanded", "[s3][xml]") {
    // A DOCTYPE is where entity expansion would be declared. Refusing to read
    // one at all disposes of the whole family of attacks without a special case
    // for each.
    REQUIRE_THROWS_AS(parseXml("<!DOCTYPE d [<!ENTITY x \"boom\">]><d>&x;</d>"), XmlParseError);

    // An entity we did not declare cannot be resolved, so passing it through
    // silently would be worse than failing.
    REQUIRE_THROWS_AS(parseXml("<d>&unknown;</d>"), XmlParseError);

    std::string deep;
    for (std::size_t i = 0; i < kMaxXmlDepth + 5; ++i) deep += "<a>";
    REQUIRE_THROWS_AS(parseXml(deep), XmlParseError);

    REQUIRE_THROWS_AS(parseXml(std::string(kMaxXmlBytes + 1, 'x')), XmlParseError);
}

TEST_CASE("malformed documents are refused", "[s3][xml]") {
    REQUIRE_THROWS_AS(parseXml(""), XmlParseError);
    REQUIRE_THROWS_AS(parseXml("   "), XmlParseError);
    REQUIRE_THROWS_AS(parseXml("not xml"), XmlParseError);
    REQUIRE_THROWS_AS(parseXml("<a>"), XmlParseError);
    REQUIRE_THROWS_AS(parseXml("<a></b>"), XmlParseError);
    REQUIRE_THROWS_AS(parseXml("<a></a><b></b>"), XmlParseError);
    REQUIRE_THROWS_AS(parseXml("<a attr></a>"), XmlParseError);
    REQUIRE_THROWS_AS(parseXml("<a attr=unquoted></a>"), XmlParseError);
    REQUIRE_THROWS_AS(parseXml("<a><!-- unterminated"), XmlParseError);
    REQUIRE_THROWS_AS(parseXml("<a><![CDATA[unterminated</a>"), XmlParseError);
}

TEST_CASE("a missing child reads as empty rather than throwing", "[s3][xml]") {
    const XmlNode root = parseXml("<Delete><Object><Key>k</Key></Object></Delete>");

    // Handlers branch on the value, so an absent element must not be an
    // exception the caller has to guard every access with.
    REQUIRE(root.child("Quiet") == nullptr);
    REQUIRE(root.childText("Quiet").empty());
    REQUIRE(root.childrenNamed("Nothing").empty());
}
