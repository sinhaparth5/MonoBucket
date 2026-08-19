#include "server/policy_reconcile.hpp"

#include "core/logging.hpp"
#include "s3/bucket_policy.hpp"

namespace monobucket {

PolicyReconciliation reconcileBucketPolicies(StorageEngine& storage) {
    PolicyReconciliation report;

    for (const BucketRecord& bucket : storage.listBuckets()) {
        if (bucket.policy.empty()) continue;
        ++report.examined;

        const s3::PolicyAnalysis analysis = s3::analyseBucketPolicy(bucket.policy, bucket.name);

        // A document this build cannot read grants nothing until it is
        // replaced. It is kept rather than deleted: it is the operator's text,
        // it is what GetBucketPolicy has always returned, and dropping it would
        // destroy the only record of what they meant.
        const bool readObjects = analysis.understood() && analysis.grants.readObjects;
        const bool listBucket  = analysis.understood() && analysis.grants.listBucket;

        if (readObjects == bucket.publicRead && listBucket == bucket.publicList) continue;

        storage.setBucketPolicy(bucket.name, bucket.policy, readObjects, listBucket);

        if (!analysis.understood()) {
            report.unenforceable.push_back(bucket.name);
            log::warn("bucket '", bucket.name, "' has a stored policy containing ",
                      analysis.unsupported,
                      ", which this build does not evaluate; the bucket now grants no anonymous "
                      "access and the document is unchanged — replace it to restore the access "
                      "you intended");
        } else {
            report.narrowed.push_back(bucket.name);
            log::warn("bucket '", bucket.name, "' re-read its stored policy: anonymous object "
                      "reads ", readObjects ? "on" : "off", ", anonymous listing ",
                      listBucket ? "on" : "off");
        }
    }

    return report;
}

}  // namespace monobucket
