#include "s3/bucket_policy.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <vector>

#include "s3/s3_error.hpp"

namespace monobucket::s3 {
namespace {

constexpr std::size_t kMaxPolicyBytes = 20 * 1024;

/// The ARN prefix every S3 resource carries. There is no partition to vary:
/// this is not AWS, and a policy naming `aws-cn` is one written for somewhere
/// else.
constexpr std::string_view kArnPrefix = "arn:aws:s3:::";

/// What a `Resource` entry addresses. A policy is attached to one bucket, so
/// there is nothing to say about any other.
enum class Scope {
    Bucket,   ///< the bucket itself — what ListBucket acts on
    Objects,  ///< every object in it — what GetObject acts on
    Both,     ///< a bare `*`
};

/// Reads one element that S3 permits to be either a string or an array of
/// them, and calls `visit` for each. Returns false if it is neither, or if any
/// member is not a string — a shape we cannot read is not a shape to guess at.
template <typename Visit>
bool forEachString(const nlohmann::json& field, Visit&& visit) {
    if (field.is_string()) {
        visit(field.get<std::string>());
        return true;
    }
    if (!field.is_array() || field.empty()) return false;
    for (const auto& item : field) {
        if (!item.is_string()) return false;
        visit(item.get<std::string>());
    }
    return true;
}

std::string inQuotes(std::string_view value) {
    return "\"" + std::string(value) + "\"";
}

}  // namespace

PolicyAnalysis analyseBucketPolicy(std::string_view document, std::string_view bucket) {
    PolicyAnalysis analysis;

    const auto refuse = [&analysis](std::string what) {
        // First one wins. Naming every problem in a document that is wrong in
        // several ways reads as a list of things to fix one at a time, when
        // the answer is that the document as a whole is outside the grammar.
        if (analysis.unsupported.empty()) analysis.unsupported = std::move(what);
    };

    nlohmann::json policy;
    try {
        policy = nlohmann::json::parse(document);
    } catch (const nlohmann::json::exception&) {
        analysis.unsupported = "a document that is not valid JSON";
        return analysis;
    }

    if (!policy.is_object()) {
        analysis.unsupported = "a document that is not a JSON object";
        return analysis;
    }

    // `Version` is a dated grammar identifier, not a document version. A value
    // this build has never seen describes a grammar it cannot be sure it is
    // reading correctly.
    if (policy.contains("Version")) {
        const auto& version = policy["Version"];
        if (!version.is_string() ||
            (version.get<std::string>() != "2012-10-17" &&
             version.get<std::string>() != "2008-10-17")) {
            analysis.unsupported = "a Version other than \"2012-10-17\"";
            return analysis;
        }
    }

    if (!policy.contains("Statement")) {
        analysis.unsupported = "a document with no Statement";
        return analysis;
    }

    // A single statement may be written as a bare object. Normalised here so
    // the loop below has one shape to read.
    std::vector<nlohmann::json> statements;
    if (policy["Statement"].is_array()) {
        for (const auto& statement : policy["Statement"]) statements.push_back(statement);
    } else if (policy["Statement"].is_object()) {
        statements.push_back(policy["Statement"]);
    } else {
        analysis.unsupported = "a Statement that is neither an object nor an array";
        return analysis;
    }

    AnonymousGrants grants;

    for (const auto& statement : statements) {
        if (!statement.is_object()) {
            refuse("a statement that is not an object");
            break;
        }

        // --- Elements that change the meaning of everything else ------------
        //
        // Each of these narrows or inverts a grant. Ignoring one turns a
        // policy that restricts into a policy that does not, which is the
        // whole failure this grammar exists to prevent.

        if (statement.contains("Condition")) {
            refuse("a Condition block");
            break;
        }
        for (const char* inverted : {"NotAction", "NotResource", "NotPrincipal"}) {
            if (statement.contains(inverted)) {
                refuse(std::string("a ") + inverted + " element");
                break;
            }
        }
        if (!analysis.unsupported.empty()) break;

        const std::string effect = statement.value("Effect", std::string{});
        if (effect == "Deny") {
            // The one rule from the real evaluator that cannot be optional is
            // that Deny wins. Honouring it against a single bucket-wide flag
            // is not possible — a Deny is nearly always scoped to part of the
            // bucket, and the flag has no way to say "all but this". So it is
            // refused at the door instead: an operator who writes one finds
            // out now, rather than discovering later that the exclusion they
            // wrote was never in force.
            refuse("a Deny statement");
            break;
        }
        if (effect != "Allow") {
            refuse("an Effect other than \"Allow\" or \"Deny\"");
            break;
        }

        // --- Principal ------------------------------------------------------

        if (!statement.contains("Principal")) {
            refuse("a statement with no Principal");
            break;
        }

        const auto& principal = statement["Principal"];
        bool        everyone  = false;
        if (principal.is_string()) {
            everyone = principal.get<std::string>() == "*";
        } else if (principal.is_object() && principal.size() == 1 && principal.contains("AWS")) {
            bool all = true;
            const bool readable = forEachString(principal["AWS"], [&all](const std::string& value) {
                if (value != "*") all = false;
            });
            everyone = readable && all;
        }
        if (!everyone) {
            // Not a gap to fill later. Policies here decide one thing —
            // whether a request carrying no credentials is served — and a
            // request that does carry them is authorised by the role of the
            // key's owner without ever consulting this document. A named
            // principal therefore has no surface to act on.
            refuse("a Principal other than \"*\"");
            break;
        }

        // --- Resource -------------------------------------------------------

        if (!statement.contains("Resource")) {
            refuse("a statement with no Resource");
            break;
        }

        std::vector<Scope> scopes;
        const std::string  bucketArn = std::string(kArnPrefix) + std::string(bucket);

        const bool resourcesReadable =
            forEachString(statement["Resource"], [&](const std::string& value) {
                if (value == "*") {
                    scopes.push_back(Scope::Both);
                    return;
                }
                if (value == bucketArn) {
                    scopes.push_back(Scope::Bucket);
                    return;
                }
                if (value == bucketArn + "/*") {
                    scopes.push_back(Scope::Objects);
                    return;
                }
                if (value.rfind(bucketArn + "/", 0) == 0) {
                    // A prefix-scoped resource such as `.../public/*`. Honest
                    // support means deciding per key at request time, which the
                    // stored flag cannot express; accepting it would publish
                    // either everything or nothing, and both are wrong in a way
                    // the document does not warn about.
                    refuse("a Resource scoped to part of a bucket (" + inQuotes(value) + ")");
                    return;
                }
                refuse("a Resource naming something other than this bucket (" + inQuotes(value) +
                       ")");
            });

        if (!resourcesReadable) refuse("a Resource that is neither a string nor a list of them");
        if (!analysis.unsupported.empty()) break;

        // --- Action ---------------------------------------------------------

        if (!statement.contains("Action")) {
            refuse("a statement with no Action");
            break;
        }

        // Actions are matched against what an anonymous client can actually be
        // served. Anything else — a write, a versioned read, an administrative
        // call — is refused rather than accepted and quietly dropped, because
        // an operator who grants anonymous PutObject and is told nothing will
        // reasonably believe it worked.
        bool       objectRead = false;
        bool       bucketList = false;
        const bool actionsReadable =
            forEachString(statement["Action"], [&](const std::string& value) {
                if (value == "*" || value == "s3:*") {
                    objectRead = true;
                    bucketList = true;
                    return;
                }
                if (value == "s3:GetObject") {
                    objectRead = true;
                    return;
                }
                if (value == "s3:ListBucket") {
                    bucketList = true;
                    return;
                }
                refuse("the action " + inQuotes(value));
            });

        if (!actionsReadable) refuse("an Action that is neither a string nor a list of them");
        if (!analysis.unsupported.empty()) break;

        // An action grants only where its resource reaches: GetObject named
        // against the bucket ARN addresses nothing, and ListBucket named
        // against the objects likewise. S3 treats both as inert, and so do we.
        for (const Scope scope : scopes) {
            if (objectRead && (scope == Scope::Objects || scope == Scope::Both)) {
                grants.readObjects = true;
            }
            if (bucketList && (scope == Scope::Bucket || scope == Scope::Both)) {
                grants.listBucket = true;
            }
        }
    }

    if (!analysis.unsupported.empty()) return analysis;

    analysis.grants = grants;
    return analysis;
}

void validateBucketPolicy(const std::string& document, std::string_view bucket) {
    if (document.empty()) {
        throw S3Exception(S3ErrorCode::InvalidArgument, "The policy document is empty.");
    }
    if (document.size() > kMaxPolicyBytes) {
        throw S3Exception(S3ErrorCode::InvalidArgument,
                          "The policy document exceeds the maximum size of 20 KB.");
    }

    const PolicyAnalysis analysis = analyseBucketPolicy(document, bucket);
    if (analysis.understood()) return;

    // Named, and refused rather than stored. A policy this server accepts is a
    // policy this server enforces; storing one it cannot read would hand back
    // a document on request that describes access nobody is being held to.
    throw S3Exception(S3ErrorCode::InvalidArgument,
                      "This policy contains " + analysis.unsupported +
                          ", which MonoBucket does not evaluate. Bucket policies here grant "
                          "anonymous s3:GetObject or s3:ListBucket to Principal \"*\" over a "
                          "whole bucket, and nothing else; signed requests are authorised by "
                          "the role of the access key's owner and never by this document.");
}

}  // namespace monobucket::s3
