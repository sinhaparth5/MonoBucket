#include <catch2/catch_test_macros.hpp>

#include "s3/cors.hpp"
#include "s3/s3_error.hpp"
#include "storage/codec.hpp"

using namespace monobucket;
using namespace monobucket::s3;

namespace {

CorsRule rule(std::vector<std::string> origins, std::vector<std::string> methods,
              std::vector<std::string> headers = {}) {
    CorsRule out;
    out.allowedOrigins = std::move(origins);
    out.allowedMethods = std::move(methods);
    out.allowedHeaders = std::move(headers);
    return out;
}

CorsQuery query(std::string origin, std::string method, std::vector<std::string> headers = {}) {
    CorsQuery out;
    out.origin           = std::move(origin);
    out.method           = std::move(method);
    out.requestedHeaders = std::move(headers);
    return out;
}

}  // namespace

TEST_CASE("origin patterns match the way S3 says they do", "[s3][cors]") {
    REQUIRE(corsPatternMatches("https://app.example.com", "https://app.example.com"));
    REQUIRE_FALSE(corsPatternMatches("https://app.example.com", "https://evil.example.com"));

    // The wildcard stands for a run of characters, not for a DNS label: S3 does
    // not treat a dot as a boundary, so `https://*.example.com` matches a
    // multi-label host too.
    REQUIRE(corsPatternMatches("https://*.example.com", "https://app.example.com"));
    REQUIRE(corsPatternMatches("https://*.example.com", "https://a.b.example.com"));
    REQUIRE_FALSE(corsPatternMatches("https://*.example.com", "https://example.com"));

    // A scheme mismatch is a mismatch. This is the case that stops a page
    // served over plain HTTP borrowing an https origin's grant.
    REQUIRE_FALSE(corsPatternMatches("https://*.example.com", "http://app.example.com"));

    REQUIRE(corsPatternMatches("*", "https://anything.invalid"));
    REQUIRE(corsPatternMatches("*", ""));

    // The two halves of a pattern must not consume the same characters, or
    // `https://a*a.com` would match a value too short to contain both.
    REQUIRE_FALSE(corsPatternMatches("https://a*a.com", "https://a.com"));
}

TEST_CASE("the first matching rule wins, in document order", "[s3][cors]") {
    std::vector<CorsRule> rules{
        rule({"https://app.example.com"}, {"GET"}),
        rule({"*"}, {"GET", "PUT", "DELETE"}),
    };

    // The narrow rule is listed first, so it answers — and its method list is
    // what the browser is told, not the permissive one below it.
    const CorsDecision first = matchCors(rules, query("https://app.example.com", "GET"));
    REQUIRE(first);
    REQUIRE(first.rule->allowedMethods == std::vector<std::string>{"GET"});

    // A method the first rule does not name falls through to the second rather
    // than failing: rule order selects, it does not shadow.
    const CorsDecision second = matchCors(rules, query("https://app.example.com", "PUT"));
    REQUIRE(second);
    REQUIRE(second.rule->allowedMethods.size() == 3);

    REQUIRE_FALSE(matchCors(rules, query("https://app.example.com", "POST")));
    REQUIRE_FALSE(matchCors({}, query("https://app.example.com", "GET")));
}

TEST_CASE("a rule must permit every header the preflight asked about", "[s3][cors]") {
    std::vector<CorsRule> rules{
        rule({"*"}, {"PUT"}, {"content-type"}),
        rule({"*"}, {"PUT"}, {"*"}),
    };

    const CorsDecision narrow =
        matchCors(rules, query("https://a.invalid", "PUT", {"content-type"}));
    REQUIRE(narrow);
    REQUIRE(narrow.allowedHeaders == std::vector<std::string>{"content-type"});

    // The first rule permits the origin and the method but not x-amz-acl, so it
    // does not match at all. Answering from it with a partial header list would
    // let the browser send a request this configuration never allowed.
    const CorsDecision wide =
        matchCors(rules, query("https://a.invalid", "PUT", {"content-type", "x-amz-acl"}));
    REQUIRE(wide);
    REQUIRE(wide.rule->allowedHeaders == std::vector<std::string>{"*"});
    REQUIRE(wide.allowedHeaders.size() == 2);

    // A rule with no AllowedHeader permits none, which is not the same as
    // permitting all.
    std::vector<CorsRule> bare{rule({"*"}, {"PUT"})};
    REQUIRE(matchCors(bare, query("https://a.invalid", "PUT")));
    REQUIRE_FALSE(matchCors(bare, query("https://a.invalid", "PUT", {"content-type"})));
}

TEST_CASE("request header lists are split and folded to lower case", "[s3][cors]") {
    REQUIRE(splitRequestedHeaders("Content-Type, X-Amz-Date") ==
            std::vector<std::string>{"content-type", "x-amz-date"});

    // Browsers vary in their spacing and at least one sends a trailing comma.
    REQUIRE(splitRequestedHeaders("content-type,,  x-amz-acl ,") ==
            std::vector<std::string>{"content-type", "x-amz-acl"});
    REQUIRE(splitRequestedHeaders("").empty());
    REQUIRE(splitRequestedHeaders("   ").empty());
}

TEST_CASE("a CORS configuration round-trips through XML", "[s3][cors]") {
    const std::string document = R"(<CORSConfiguration>
  <CORSRule>
    <ID>web</ID>
    <AllowedOrigin>https://app.example.com</AllowedOrigin>
    <AllowedMethod>get</AllowedMethod>
    <AllowedMethod>PUT</AllowedMethod>
    <AllowedHeader>Content-Type</AllowedHeader>
    <ExposeHeader>ETag</ExposeHeader>
    <MaxAgeSeconds>3000</MaxAgeSeconds>
  </CORSRule>
</CORSConfiguration>)";

    const std::vector<CorsRule> rules = parseCorsConfiguration(document);
    REQUIRE(rules.size() == 1);
    REQUIRE(rules[0].id == "web");

    // Methods are uppercased and headers lowercased on the way in, so matching
    // never has to say that either comparison is case-insensitive.
    REQUIRE(rules[0].allowedMethods == std::vector<std::string>{"GET", "PUT"});
    REQUIRE(rules[0].allowedHeaders == std::vector<std::string>{"content-type"});

    // ExposeHeader keeps its case: it is echoed to the browser, not compared.
    REQUIRE(rules[0].exposeHeaders == std::vector<std::string>{"ETag"});
    REQUIRE(rules[0].maxAgeSeconds == 3000);

    const std::vector<CorsRule> again = parseCorsConfiguration(renderCorsConfiguration(rules));
    REQUIRE(again.size() == 1);
    REQUIRE(again[0].id == rules[0].id);
    REQUIRE(again[0].allowedOrigins == rules[0].allowedOrigins);
    REQUIRE(again[0].allowedMethods == rules[0].allowedMethods);
    REQUIRE(again[0].allowedHeaders == rules[0].allowedHeaders);
    REQUIRE(again[0].exposeHeaders == rules[0].exposeHeaders);
    REQUIRE(again[0].maxAgeSeconds == rules[0].maxAgeSeconds);
}

TEST_CASE("a rule with no max age renders without the element", "[s3][cors]") {
    const std::string rendered = renderCorsConfiguration({rule({"*"}, {"GET"})});
    REQUIRE(rendered.find("MaxAgeSeconds") == std::string::npos);

    // Zero is a real answer — "do not cache this preflight" — and must survive
    // the round trip as itself rather than as absence.
    CorsRule zero      = rule({"*"}, {"GET"});
    zero.maxAgeSeconds = 0;
    REQUIRE(renderCorsConfiguration({zero}).find("<MaxAgeSeconds>0</MaxAgeSeconds>") !=
            std::string::npos);
    REQUIRE(parseCorsConfiguration(renderCorsConfiguration({zero}))[0].maxAgeSeconds == 0);
}

TEST_CASE("a configuration S3 would refuse is refused here", "[s3][cors]") {
    const auto refused = [](const char* document) {
        REQUIRE_THROWS_AS(parseCorsConfiguration(document), S3Exception);
    };

    refused("");
    refused("<CORSConfiguration></CORSConfiguration>");
    refused("<NotCORSConfiguration><CORSRule/></NotCORSConfiguration>");

    // A rule missing either half of the match is not a rule that permits
    // nothing — it is a document the author got wrong, and saying so now beats
    // a mystery 403 later.
    refused("<CORSConfiguration><CORSRule><AllowedMethod>GET</AllowedMethod></CORSRule>"
            "</CORSConfiguration>");
    refused("<CORSConfiguration><CORSRule><AllowedOrigin>*</AllowedOrigin></CORSRule>"
            "</CORSConfiguration>");

    // A method S3 does not accept in a configuration. Dropping the rule
    // silently would surface as a preflight failure somewhere else entirely.
    refused("<CORSConfiguration><CORSRule><AllowedOrigin>*</AllowedOrigin>"
            "<AllowedMethod>PATCH</AllowedMethod></CORSRule></CORSConfiguration>");

    refused("<CORSConfiguration><CORSRule><AllowedOrigin>ht*tp://*.a</AllowedOrigin>"
            "<AllowedMethod>GET</AllowedMethod></CORSRule></CORSConfiguration>");

    refused("<CORSConfiguration><CORSRule><AllowedOrigin>*</AllowedOrigin>"
            "<AllowedMethod>GET</AllowedMethod><MaxAgeSeconds>86401</MaxAgeSeconds></CORSRule>"
            "</CORSConfiguration>");
    refused("<CORSConfiguration><CORSRule><AllowedOrigin>*</AllowedOrigin>"
            "<AllowedMethod>GET</AllowedMethod><MaxAgeSeconds>-1</MaxAgeSeconds></CORSRule>"
            "</CORSConfiguration>");
    refused("<CORSConfiguration><CORSRule><AllowedOrigin>*</AllowedOrigin>"
            "<AllowedMethod>GET</AllowedMethod><MaxAgeSeconds>soon</MaxAgeSeconds></CORSRule>"
            "</CORSConfiguration>");
}

TEST_CASE("more rules than S3 accepts are refused rather than truncated", "[s3][cors]") {
    std::string document = "<CORSConfiguration>";
    for (int i = 0; i < 101; ++i) {
        document += "<CORSRule><AllowedOrigin>*</AllowedOrigin>"
                    "<AllowedMethod>GET</AllowedMethod></CORSRule>";
    }
    document += "</CORSConfiguration>";

    REQUIRE_THROWS_AS(parseCorsConfiguration(document), S3Exception);
}

TEST_CASE("CORS rules survive the record encoding", "[s3][cors]") {
    std::vector<CorsRule> rules{rule({"https://a.invalid", "https://*.b.invalid"},
                                     {"GET", "PUT"}, {"content-type"})};
    rules[0].id            = "web";
    rules[0].exposeHeaders = {"ETag", "x-amz-request-id"};
    rules[0].maxAgeSeconds = 600;
    rules.push_back(rule({"*"}, {"HEAD"}));

    std::string   encoded;
    codec::Writer writer(encoded);
    encodeCorsRules(writer, rules);

    codec::Reader               reader(encoded);
    const std::vector<CorsRule> back = decodeCorsRules(reader);

    REQUIRE(back.size() == 2);
    REQUIRE(back[0].id == "web");
    REQUIRE(back[0].allowedOrigins == rules[0].allowedOrigins);
    REQUIRE(back[0].allowedMethods == rules[0].allowedMethods);
    REQUIRE(back[0].allowedHeaders == rules[0].allowedHeaders);
    REQUIRE(back[0].exposeHeaders == rules[0].exposeHeaders);
    REQUIRE(back[0].maxAgeSeconds == 600);

    // Absence must not decode as zero: the two mean different things to a
    // browser, and the flag is what keeps them apart.
    REQUIRE(back[1].maxAgeSeconds == -1);

    // The whole field was consumed, which is what lets it be appended to the
    // bucket record without a version bump.
    REQUIRE(reader.exhausted());

    std::string   emptyEncoded;
    codec::Writer emptyWriter(emptyEncoded);
    encodeCorsRules(emptyWriter, {});
    codec::Reader emptyReader(emptyEncoded);
    REQUIRE(decodeCorsRules(emptyReader).empty());
}
