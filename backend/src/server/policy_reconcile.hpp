#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "storage/storage_engine.hpp"

// Bringing stored bucket policies into line with what this build enforces.
//
// A free function rather than a Server member for the same reason the identity
// migration is one: it rewrites records somebody else already has, so it has to
// be testable against a store rather than against a listener.

namespace monobucket {

/// What the pass found. Reported so a log line can say which buckets changed
/// and why, rather than leaving an operator to notice that a bucket stopped
/// being public.
struct PolicyReconciliation {
    std::size_t examined = 0;

    /// Buckets whose enforced access no longer matches what the flags said.
    std::vector<std::string> narrowed;

    /// Buckets whose stored policy this build will not evaluate. The document
    /// is left exactly as written — it is the operator's text and removing it
    /// would destroy the only record of what they intended — but it grants
    /// nothing until they replace it.
    std::vector<std::string> unenforceable;
};

/// Re-derives every bucket's anonymous access from its stored policy.
///
/// Runs at startup because the flags are what the request path consults and
/// they are derived data: a build that reads a document differently from the
/// build that stored it would otherwise keep enforcing the old reading
/// forever. That is not hypothetical — a policy whose Deny was silently
/// ignored is stored with the bucket marked public, and only re-reading it
/// closes that.
///
/// Buckets with no policy are left alone: their access came from the console
/// toggle, not from a document, and there is nothing to re-derive.
PolicyReconciliation reconcileBucketPolicies(StorageEngine& storage);

}  // namespace monobucket
