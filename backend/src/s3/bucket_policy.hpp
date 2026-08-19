#pragma once

#include <string>
#include <string_view>

// Bucket policies, reduced to the only question this server can answer with
// them: what may a client with no credentials at all do here?
//
// Deliberately not an IAM evaluator. There is no principal model to evaluate
// against — a signed request is authorised by the role of the key's owner and
// never consults the policy — so the whole of the policy language that talks
// about *who* is either "everyone" or unusable. What is left is a small,
// closed grammar, and the rule this module exists to enforce is that a
// document outside that grammar is refused rather than stored and ignored.
//
// The evaluation is expressed over the document text, not over Drogon types,
// so the whole grammar is testable without a socket. Keep it that way.

namespace monobucket::s3 {

/// What a policy grants to an unauthenticated client.
///
/// Two flags rather than one, because "public" is two different exposures and
/// a policy says which it means. Granting `s3:GetObject` publishes the objects
/// somebody already knows the names of; granting `s3:ListBucket` publishes the
/// names themselves. Collapsing them loses the distinction in the unsafe
/// direction, which is what this server used to do.
struct AnonymousGrants {
    /// `s3:GetObject` over every object in the bucket.
    bool readObjects = false;

    /// `s3:ListBucket` on the bucket itself — anonymous enumeration of keys.
    bool listBucket = false;

    bool any() const noexcept { return readObjects || listBucket; }
};

/// The outcome of reading a document: what it grants, or what stopped us.
struct PolicyAnalysis {
    AnonymousGrants grants;

    /// Names the first element this build cannot evaluate, phrased to drop
    /// into a sentence. Empty when the document was understood in full.
    ///
    /// A non-empty value makes `grants` meaningless: a document that is only
    /// partly understood cannot be partly applied, because the part not
    /// understood is exactly as likely to have been a restriction.
    std::string unsupported;

    bool understood() const noexcept { return unsupported.empty(); }
};

/// Reads `document` as a policy attached to `bucket`. Never throws.
PolicyAnalysis analyseBucketPolicy(std::string_view document, std::string_view bucket);

/// Throws `S3Exception(InvalidArgument)` unless the document is well formed,
/// within the size limit, and understood in full.
void validateBucketPolicy(const std::string& document, std::string_view bucket);

}  // namespace monobucket::s3
