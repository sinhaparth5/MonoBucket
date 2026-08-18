#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "storage/blob_store.hpp"
#include "storage/metadata_store.hpp"
#include "temporary_directory.hpp"

using monobucket::BucketRecord;
using monobucket::ListObjectsRequest;
using monobucket::MetadataStore;
using monobucket::MetadataStoreOptions;
using monobucket::ObjectRecord;
using monobucket::PartRecord;
using monobucket::StorageError;
using monobucket::StorageErrorCode;
using monobucket::UploadRecord;
using monobucket::testing::TemporaryDirectory;

namespace {

std::unique_ptr<MetadataStore> openStore(const TemporaryDirectory& root) {
    MetadataStoreOptions options;
    options.path              = (root.path() / "meta").string();
    options.memoryBudgetBytes = 8ull * 1024 * 1024;
    return monobucket::openRocksMetadataStore(options);
}

/// Blob ids become path components, so they must be well formed even in tests
/// that never touch the filesystem.
std::string blobId(char fill) { return std::string(32, fill); }

ObjectRecord object(std::string key, std::string blob, std::uint64_t size = 10) {
    ObjectRecord record;
    record.key          = std::move(key);
    record.blobId       = std::move(blob);
    record.size         = size;
    record.etag         = "etag";
    record.sha256       = "sha";
    record.lastModified = monobucket::nowMs();
    return record;
}

void createBucket(MetadataStore& store, const std::string& name) {
    BucketRecord bucket;
    bucket.name      = name;
    bucket.createdAt = monobucket::nowMs();
    store.createBucket(bucket);
}

std::vector<std::string> keysOf(const monobucket::ListObjectsResult& result) {
    std::vector<std::string> keys;
    for (const auto& object : result.objects) keys.push_back(object.key);
    return keys;
}

}  // namespace

TEST_CASE("buckets are created, listed and deleted", "[metadata]") {
    TemporaryDirectory root("meta-buckets");
    auto               store = openStore(root);

    createBucket(*store, "photos");
    createBucket(*store, "archive");

    // ListBuckets is specified to be ordered by name.
    const auto buckets = store->listBuckets();
    REQUIRE(buckets.size() == 2);
    CHECK(buckets[0].name == "archive");
    CHECK(buckets[1].name == "photos");

    CHECK(store->getBucket("photos").has_value());
    CHECK_FALSE(store->getBucket("missing").has_value());

    store->deleteBucket("photos");
    CHECK_FALSE(store->getBucket("photos").has_value());
    CHECK(store->usage().buckets == 1);
}

TEST_CASE("bucket errors are distinguishable", "[metadata]") {
    TemporaryDirectory root("meta-bucket-errors");
    auto               store = openStore(root);

    createBucket(*store, "photos");

    // Each of these maps onto a different S3 error code, so a shared exception
    // type would be untranslatable at the protocol boundary.
    try {
        createBucket(*store, "photos");
        FAIL("creating a duplicate bucket should throw");
    } catch (const StorageError& error) {
        CHECK(error.code() == StorageErrorCode::BucketAlreadyExists);
    }

    try {
        store->deleteBucket("missing");
        FAIL("deleting a missing bucket should throw");
    } catch (const StorageError& error) {
        CHECK(error.code() == StorageErrorCode::NoSuchBucket);
    }
}

TEST_CASE("a bucket holding objects refuses to be deleted", "[metadata]") {
    TemporaryDirectory root("meta-not-empty");
    auto               store = openStore(root);

    createBucket(*store, "photos");
    store->putObject("photos", object("cat.jpg", blobId('a')), monobucket::Durability::None);

    try {
        store->deleteBucket("photos");
        FAIL("deleting a non-empty bucket should throw");
    } catch (const StorageError& error) {
        CHECK(error.code() == StorageErrorCode::BucketNotEmpty);
    }

    store->deleteObject("photos", "cat.jpg", monobucket::Durability::None);
    CHECK_NOTHROW(store->deleteBucket("photos"));
}

TEST_CASE("writing to a missing bucket is refused", "[metadata]") {
    TemporaryDirectory root("meta-no-bucket");
    auto               store = openStore(root);

    try {
        store->putObject("nope", object("k", blobId('a')), monobucket::Durability::None);
        FAIL("putting into a missing bucket should throw");
    } catch (const StorageError& error) {
        CHECK(error.code() == StorageErrorCode::NoSuchBucket);
    }
}

TEST_CASE("objects round-trip with their metadata", "[metadata]") {
    TemporaryDirectory root("meta-objects");
    auto               store = openStore(root);

    createBucket(*store, "photos");

    ObjectRecord record = object("holiday/beach.jpg", blobId('a'), 4096);
    record.contentType  = "image/jpeg";
    record.etag         = "9d41272d35684b4be019260f886b359e";
    record.sha256       = std::string(64, 'f');
    record.userMetadata = {{"author", "someone"}, {"camera", "a model"}};

    store->putObject("photos", record, monobucket::Durability::None);

    const auto loaded = store->getObject("photos", "holiday/beach.jpg");
    REQUIRE(loaded.has_value());
    CHECK(loaded->key == record.key);
    CHECK(loaded->blobId == record.blobId);
    CHECK(loaded->size == record.size);
    CHECK(loaded->etag == record.etag);
    CHECK(loaded->sha256 == record.sha256);
    CHECK(loaded->contentType == "image/jpeg");
    CHECK(loaded->userMetadata == record.userMetadata);
    CHECK(loaded->lastModified == record.lastModified);
}

TEST_CASE("overwriting an object releases the payload it replaced", "[metadata]") {
    TemporaryDirectory root("meta-overwrite");
    auto               store = openStore(root);

    createBucket(*store, "photos");
    store->putObject("photos", object("k", blobId('a'), 100), monobucket::Durability::None);

    // The released id is the caller's only notice that a payload is now
    // unreferenced; losing it here is how an object store fills up.
    const auto outcome =
        store->putObject("photos", object("k", blobId('b'), 250), monobucket::Durability::None);
    REQUIRE(outcome.releasedBlobId.has_value());
    CHECK(*outcome.releasedBlobId == blobId('a'));

    // The size accounting must follow the replacement, not accumulate.
    CHECK(store->usage().objects == 1);
    CHECK(store->usage().bytes == 250);
}

TEST_CASE("deleting an object releases its payload", "[metadata]") {
    TemporaryDirectory root("meta-delete");
    auto               store = openStore(root);

    createBucket(*store, "photos");
    store->putObject("photos", object("k", blobId('a'), 100), monobucket::Durability::None);

    const auto outcome = store->deleteObject("photos", "k", monobucket::Durability::None);
    CHECK(outcome.existed);
    REQUIRE(outcome.releasedBlobId.has_value());
    CHECK(*outcome.releasedBlobId == blobId('a'));

    CHECK(store->usage().objects == 0);
    CHECK(store->usage().bytes == 0);

    // S3 answers 204 for a key that was never there, so this is not an error.
    const auto again = store->deleteObject("photos", "k", monobucket::Durability::None);
    CHECK_FALSE(again.existed);
    CHECK_FALSE(again.releasedBlobId.has_value());
}

TEST_CASE("a new payload stops being owed to the reclaimer once referenced",
          "[metadata][reclaim]") {
    TemporaryDirectory root("meta-track");
    auto               store = openStore(root);

    createBucket(*store, "photos");

    // The write path records the id before the payload exists, so a crash
    // leaves a trace. Publishing the object is what clears it.
    store->trackBlob(blobId('a'));
    CHECK(store->listOrphans(10, monobucket::nowMs()).size() == 1);

    store->putObject("photos", object("k", blobId('a')), monobucket::Durability::None);
    CHECK(store->listOrphans(10, monobucket::nowMs()).empty());
}

TEST_CASE("young reclamation records are left alone", "[metadata][reclaim]") {
    TemporaryDirectory root("meta-grace");
    auto               store = openStore(root);

    store->trackBlob(blobId('a'));

    // An upload still in flight looks exactly like an abandoned payload. The
    // age filter is what stops the sweeper from deleting the file out from
    // under a client that is still writing it.
    CHECK(store->listOrphans(10, monobucket::nowMs() - 60'000).empty());
    CHECK(store->listOrphans(10, monobucket::nowMs()).size() == 1);
}

TEST_CASE("listing returns keys in binary order", "[metadata][list]") {
    TemporaryDirectory root("meta-order");
    auto               store = openStore(root);

    createBucket(*store, "b");
    for (const char* key : {"z", "a/b", "a", "ab", "a/b/c"}) {
        store->putObject("b", object(key, blobId('a')), monobucket::Durability::None);
    }

    ListObjectsRequest request;
    const auto         result = store->listObjects("b", request);
    CHECK(keysOf(result) == std::vector<std::string>{"a", "a/b", "a/b/c", "ab", "z"});
    CHECK_FALSE(result.truncated);
}

TEST_CASE("a prefix restricts the listing", "[metadata][list]") {
    TemporaryDirectory root("meta-prefix");
    auto               store = openStore(root);

    createBucket(*store, "b");
    for (const char* key : {"photos/1.jpg", "photos/2.jpg", "photos2/other.jpg", "docs/a.txt"}) {
        store->putObject("b", object(key, blobId('a')), monobucket::Durability::None);
    }

    ListObjectsRequest request;
    request.prefix = "photos/";

    // `photos2/` shares a textual prefix with `photos` but not with `photos/`;
    // getting this wrong leaks one bucket's folder into another's listing.
    CHECK(keysOf(store->listObjects("b", request)) ==
          std::vector<std::string>{"photos/1.jpg", "photos/2.jpg"});
}

TEST_CASE("a delimiter rolls keys up into common prefixes", "[metadata][list]") {
    TemporaryDirectory root("meta-delimiter");
    auto               store = openStore(root);

    createBucket(*store, "b");
    for (const char* key : {"a.txt", "photos/1.jpg", "photos/2.jpg", "photos/raw/3.jpg", "z.txt"}) {
        store->putObject("b", object(key, blobId('a')), monobucket::Durability::None);
    }

    ListObjectsRequest request;
    request.delimiter = "/";

    const auto result = store->listObjects("b", request);
    CHECK(keysOf(result) == std::vector<std::string>{"a.txt", "z.txt"});
    // The whole subtree collapses to one entry, including the nested `raw/`.
    CHECK(result.commonPrefixes == std::vector<std::string>{"photos/"});
}

TEST_CASE("a delimiter applies below the prefix", "[metadata][list]") {
    TemporaryDirectory root("meta-delimiter-prefix");
    auto               store = openStore(root);

    createBucket(*store, "b");
    for (const char* key : {"photos/1.jpg", "photos/raw/a.dng", "photos/raw/b.dng",
                            "photos/edited/c.jpg"}) {
        store->putObject("b", object(key, blobId('a')), monobucket::Durability::None);
    }

    ListObjectsRequest request;
    request.prefix    = "photos/";
    request.delimiter = "/";

    const auto result = store->listObjects("b", request);
    CHECK(keysOf(result) == std::vector<std::string>{"photos/1.jpg"});
    CHECK(result.commonPrefixes ==
          std::vector<std::string>{"photos/edited/", "photos/raw/"});
}

TEST_CASE("pagination walks the whole keyspace exactly once", "[metadata][list]") {
    TemporaryDirectory root("meta-paginate");
    auto               store = openStore(root);

    createBucket(*store, "b");

    std::vector<std::string> expected;
    for (int i = 0; i < 25; ++i) {
        // Zero-padded so binary order and numeric order agree.
        std::string key = "key-" + std::string(2 - std::to_string(i).size(), '0') +
                          std::to_string(i);
        store->putObject("b", object(key, blobId('a')), monobucket::Durability::None);
        expected.push_back(key);
    }
    std::sort(expected.begin(), expected.end());

    ListObjectsRequest request;
    request.maxKeys = 7;

    std::vector<std::string> seen;
    while (true) {
        const auto page = store->listObjects("b", request);
        for (const auto& record : page.objects) seen.push_back(record.key);
        if (!page.truncated) break;

        REQUIRE_FALSE(page.nextStartAfter.empty());
        request.startAfter = page.nextStartAfter;
    }

    // No key skipped, none repeated — the property that matters to a client
    // syncing a bucket.
    CHECK(seen == expected);
}

TEST_CASE("pagination terminates when a page ends on a common prefix",
          "[metadata][list]") {
    TemporaryDirectory root("meta-paginate-prefix");
    auto               store = openStore(root);

    createBucket(*store, "b");
    for (const char* key : {"a/1", "a/2", "b/1", "b/2", "c/1", "d"}) {
        store->putObject("b", object(key, blobId('a')), monobucket::Durability::None);
    }

    ListObjectsRequest request;
    request.delimiter = "/";
    request.maxKeys   = 1;

    // Resuming from a common prefix is the case that loops forever if the
    // marker is treated as an ordinary key: `a/` sorts below `a/1`, so seeking
    // just past it lands back inside the group it was meant to skip.
    std::vector<std::string> prefixes;
    std::vector<std::string> keys;
    int                      pages = 0;

    while (pages++ < 20) {
        const auto page = store->listObjects("b", request);
        for (const auto& prefix : page.commonPrefixes) prefixes.push_back(prefix);
        for (const auto& record : page.objects) keys.push_back(record.key);
        if (!page.truncated) break;
        request.startAfter = page.nextStartAfter;
    }

    CHECK(pages < 20);  // it terminated rather than spinning
    CHECK(prefixes == std::vector<std::string>{"a/", "b/", "c/"});
    CHECK(keys == std::vector<std::string>{"d"});
}

TEST_CASE("listing an unknown bucket is refused", "[metadata][list]") {
    TemporaryDirectory root("meta-list-missing");
    auto               store = openStore(root);

    try {
        store->listObjects("nope", ListObjectsRequest{});
        FAIL("listing a missing bucket should throw");
    } catch (const StorageError& error) {
        CHECK(error.code() == StorageErrorCode::NoSuchBucket);
    }
}

TEST_CASE("multipart parts are stored and returned in ascending order",
          "[metadata][multipart]") {
    TemporaryDirectory root("meta-parts");
    auto               store = openStore(root);

    createBucket(*store, "b");

    UploadRecord upload;
    upload.uploadId  = "UPLOAD1";
    upload.bucket    = "b";
    upload.key       = "big.bin";
    upload.createdAt = monobucket::nowMs();
    store->createUpload(upload);

    // Uploaded out of order, as a parallel client would.
    for (std::uint32_t number : {3u, 1u, 10u, 2u}) {
        PartRecord part;
        part.partNumber = number;
        part.blobId     = std::string(32, static_cast<char>('a' + number % 6));
        part.size       = 5ull * 1024 * 1024;
        part.etag       = "etag" + std::to_string(number);
        part.uploadedAt = monobucket::nowMs();
        store->putPart("UPLOAD1", part, monobucket::Durability::None);
    }

    const auto parts = store->listParts("UPLOAD1");
    REQUIRE(parts.size() == 4);
    CHECK(parts[0].partNumber == 1);
    CHECK(parts[1].partNumber == 2);
    CHECK(parts[2].partNumber == 3);
    // 10 must sort after 3, which decimal string ordering would get wrong.
    CHECK(parts[3].partNumber == 10);
}

TEST_CASE("re-uploading a part releases the payload it replaced",
          "[metadata][multipart]") {
    TemporaryDirectory root("meta-part-replace");
    auto               store = openStore(root);

    createBucket(*store, "b");

    UploadRecord upload;
    upload.uploadId = "UPLOAD1";
    upload.bucket   = "b";
    upload.key      = "big.bin";
    store->createUpload(upload);

    PartRecord part;
    part.partNumber = 1;
    part.blobId     = blobId('a');
    part.size       = 1024;
    part.etag       = "first";
    store->putPart("UPLOAD1", part, monobucket::Durability::None);

    part.blobId = blobId('b');
    part.etag   = "second";
    const auto outcome = store->putPart("UPLOAD1", part, monobucket::Durability::None);

    REQUIRE(outcome.releasedBlobId.has_value());
    CHECK(*outcome.releasedBlobId == blobId('a'));
    CHECK(store->listParts("UPLOAD1").size() == 1);
}

TEST_CASE("aborting an upload releases every part", "[metadata][multipart]") {
    TemporaryDirectory root("meta-abort");
    auto               store = openStore(root);

    createBucket(*store, "b");

    UploadRecord upload;
    upload.uploadId = "UPLOAD1";
    upload.bucket   = "b";
    upload.key      = "big.bin";
    store->createUpload(upload);

    for (std::uint32_t number : {1u, 2u, 3u}) {
        PartRecord part;
        part.partNumber = number;
        part.blobId     = std::string(32, static_cast<char>('a' + number));
        part.size       = 1024;
        store->putPart("UPLOAD1", part, monobucket::Durability::None);
    }

    const auto released = store->abortUpload("UPLOAD1", monobucket::Durability::None);
    CHECK(released.size() == 3);
    CHECK_FALSE(store->getUpload("UPLOAD1").has_value());
    CHECK(store->usage().uploads == 0);

    // The bucket is empty again only because the parts were released, not
    // merely forgotten.
    CHECK_NOTHROW(store->deleteBucket("b"));
}

TEST_CASE("an unknown upload is distinguishable", "[metadata][multipart]") {
    TemporaryDirectory root("meta-no-upload");
    auto               store = openStore(root);

    try {
        store->abortUpload("NOPE", monobucket::Durability::None);
        FAIL("aborting a missing upload should throw");
    } catch (const StorageError& error) {
        CHECK(error.code() == StorageErrorCode::NoSuchUpload);
    }
}

TEST_CASE("an in-progress upload keeps its bucket alive", "[metadata][multipart]") {
    TemporaryDirectory root("meta-upload-holds-bucket");
    auto               store = openStore(root);

    createBucket(*store, "b");

    UploadRecord upload;
    upload.uploadId = "UPLOAD1";
    upload.bucket   = "b";
    upload.key      = "big.bin";
    store->createUpload(upload);

    // Deleting the bucket would strand the parts, so S3 refuses while an
    // upload is outstanding.
    try {
        store->deleteBucket("b");
        FAIL("deleting a bucket with an upload in progress should throw");
    } catch (const StorageError& error) {
        CHECK(error.code() == StorageErrorCode::BucketNotEmpty);
    }
}

TEST_CASE("counters survive a reopen", "[metadata]") {
    TemporaryDirectory root("meta-reopen");

    {
        auto store = openStore(root);
        createBucket(*store, "b");
        store->putObject("b", object("one", blobId('a'), 100), monobucket::Durability::None);
        store->putObject("b", object("two", blobId('b'), 250), monobucket::Durability::None);
        store->flush();
    }

    // Counters are maintained in memory at runtime, so a restart has to rebuild
    // them from the keyspace or `/metrics` would report zero after every deploy.
    auto reopened = openStore(root);
    const auto usage = reopened->usage();
    CHECK(usage.buckets == 1);
    CHECK(usage.objects == 2);
    CHECK(usage.bytes == 350);

    CHECK(reopened->getObject("b", "one").has_value());
}

// --- Identity ---------------------------------------------------------------

TEST_CASE("the administrator record survives a reopen", "[metadata]") {
    TemporaryDirectory root{"admin"};

    {
        auto store = openStore(root);
        CHECK_FALSE(store->getAdmin().has_value());

        monobucket::AdminRecord admin;
        admin.username     = "operator";
        admin.passwordHash = "pbkdf2-sha256$1000$aabb$ccdd";
        admin.createdAt    = 1000;
        admin.updatedAt    = 2000;
        store->putAdmin(admin);
    }

    auto store = openStore(root);
    auto admin = store->getAdmin();
    REQUIRE(admin.has_value());
    CHECK(admin->username == "operator");
    CHECK(admin->passwordHash == "pbkdf2-sha256$1000$aabb$ccdd");
    CHECK(admin->createdAt == 1000);
    CHECK(admin->updatedAt == 2000);
}

TEST_CASE("provisioning again replaces the administrator", "[metadata]") {
    TemporaryDirectory root{"admin-reset"};
    auto               store = openStore(root);

    monobucket::AdminRecord admin;
    admin.username     = "admin";
    admin.passwordHash = "first";
    store->putAdmin(admin);

    // There is one account, so this is a write rather than an insert — a
    // second record would leave the store with two answers to one question.
    admin.username     = "renamed";
    admin.passwordHash = "second";
    store->putAdmin(admin);

    const auto stored = store->getAdmin();
    REQUIRE(stored.has_value());
    CHECK(stored->username == "renamed");
    CHECK(stored->passwordHash == "second");
}

TEST_CASE("an access key round-trips through the store", "[metadata]") {
    TemporaryDirectory root{"access-keys"};

    {
        auto store = openStore(root);

        monobucket::AccessKeyRecord key;
        key.accessKeyId = "MBAAAAAAAAAAAAAAAAAA";
        key.secretKey   = "a-secret-worth-forty-characters-exactly1";
        key.description = "backups from the nightly job";
        key.createdAt   = 4242;
        store->putAccessKey(key);
    }

    auto       store = openStore(root);
    const auto key   = store->getAccessKey("MBAAAAAAAAAAAAAAAAAA");
    REQUIRE(key.has_value());
    CHECK(key->accessKeyId == "MBAAAAAAAAAAAAAAAAAA");
    CHECK(key->secretKey == "a-secret-worth-forty-characters-exactly1");
    CHECK(key->description == "backups from the nightly job");
    CHECK(key->createdAt == 4242);
    CHECK(key->rotatedAt == 0);
}

TEST_CASE("an access key that was never issued resolves to nothing", "[metadata]") {
    TemporaryDirectory root{"access-keys-missing"};
    auto               store = openStore(root);

    CHECK_FALSE(store->getAccessKey("MBAAAAAAAAAAAAAAAAAA").has_value());
    CHECK_FALSE(store->getAccessKey("").has_value());
}

TEST_CASE("access keys list in id order", "[metadata]") {
    TemporaryDirectory root{"access-keys-list"};
    auto               store = openStore(root);

    for (const char* id : {"MBCCCCCCCCCCCCCCCCCC", "MBAAAAAAAAAAAAAAAAAA",
                           "MBBBBBBBBBBBBBBBBBBB"}) {
        monobucket::AccessKeyRecord key;
        key.accessKeyId = id;
        key.secretKey   = "secret";
        store->putAccessKey(key);
    }

    const auto keys = store->listAccessKeys();
    REQUIRE(keys.size() == 3);
    CHECK(keys[0].accessKeyId == "MBAAAAAAAAAAAAAAAAAA");
    CHECK(keys[1].accessKeyId == "MBBBBBBBBBBBBBBBBBBB");
    CHECK(keys[2].accessKeyId == "MBCCCCCCCCCCCCCCCCCC");
}

TEST_CASE("rotation keeps the id and replaces the secret", "[metadata]") {
    TemporaryDirectory root{"access-keys-rotate"};
    auto               store = openStore(root);

    monobucket::AccessKeyRecord key;
    key.accessKeyId = "MBAAAAAAAAAAAAAAAAAA";
    key.secretKey   = "the-original-secret";
    key.createdAt   = 100;
    store->putAccessKey(key);

    key.secretKey = "the-replacement-secret";
    key.rotatedAt = 500;
    store->putAccessKey(key);

    const auto stored = store->getAccessKey("MBAAAAAAAAAAAAAAAAAA");
    REQUIRE(stored.has_value());
    CHECK(stored->secretKey == "the-replacement-secret");
    CHECK(stored->createdAt == 100);
    CHECK(stored->rotatedAt == 500);
    CHECK(store->listAccessKeys().size() == 1);
}

TEST_CASE("revoking an access key removes it entirely", "[metadata]") {
    TemporaryDirectory root{"access-keys-revoke"};

    {
        auto store = openStore(root);

        monobucket::AccessKeyRecord key;
        key.accessKeyId = "MBAAAAAAAAAAAAAAAAAA";
        key.secretKey   = "a-secret";
        store->putAccessKey(key);

        CHECK(store->deleteAccessKey("MBAAAAAAAAAAAAAAAAAA"));
        CHECK_FALSE(store->getAccessKey("MBAAAAAAAAAAAAAAAAAA").has_value());

        // False rather than throwing: revoking twice is what a double-clicked
        // button does, and the second one has nothing to report.
        CHECK_FALSE(store->deleteAccessKey("MBAAAAAAAAAAAAAAAAAA"));
    }

    // And it stays revoked. A delete that only cleared a cache would let a
    // restart resurrect the credential.
    auto store = openStore(root);
    CHECK_FALSE(store->getAccessKey("MBAAAAAAAAAAAAAAAAAA").has_value());
    CHECK(store->listAccessKeys().empty());
}

TEST_CASE("credentials do not appear in the bucket or object keyspace", "[metadata]") {
    TemporaryDirectory root{"access-keys-isolated"};
    auto               store = openStore(root);

    monobucket::AccessKeyRecord key;
    key.accessKeyId = "MBAAAAAAAAAAAAAAAAAA";
    key.secretKey   = "a-secret";
    store->putAccessKey(key);

    monobucket::AdminRecord admin;
    admin.username     = "admin";
    admin.passwordHash = "hash";
    store->putAdmin(admin);

    // Both live under their own type tags, so a ListBuckets cannot enumerate
    // them and a bucket named like a key id cannot shadow one.
    CHECK(store->listBuckets().empty());
    CHECK(store->usage().buckets == 0);
}
