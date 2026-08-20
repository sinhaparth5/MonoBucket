#include "storage/checkpoint.hpp"

#include <cerrno>
#include <chrono>
#include <system_error>

#include "core/logging.hpp"
#include "storage/blob_store.hpp"
#include "storage/metadata_store.hpp"

namespace monobucket {
namespace {

namespace fs = std::filesystem;

[[noreturn]] void fail(StorageErrorCode code, const std::string& what) {
    throw StorageError(code, what);
}

std::int64_t millisSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

}  // namespace

void requireUsableDestination(const fs::path& destination) {
    if (destination.empty()) fail(StorageErrorCode::Internal, "a destination is required");

    std::error_code ec;
    if (fs::exists(destination, ec)) {
        if (!fs::is_directory(destination, ec)) {
            fail(StorageErrorCode::Io,
                 "'" + destination.string() + "' already exists and is not a directory");
        }
        // Non-empty is refused rather than merged. RocksDB will not check
        // point into a directory that exists at all, and half of one backup
        // beside half of another is the kind of thing nobody discovers until
        // they try to restore it.
        if (!fs::is_empty(destination, ec)) {
            fail(StorageErrorCode::Io, "'" + destination.string() + "' is not empty");
        }
    }

    const fs::path parent = destination.parent_path().empty() ? fs::path(".")
                                                              : destination.parent_path();
    if (!fs::exists(parent, ec)) {
        fail(StorageErrorCode::Io, "'" + parent.string() + "' does not exist");
    }

    // Proved by writing rather than by asking. access(2) answers for the real
    // uid and a read-only mount answers for neither, so the only reliable test
    // of "can I write here" is to write here.
    const fs::path probe = parent / (".monobucket-checkpoint-probe");
    fs::remove(probe, ec);
    {
        std::error_code created;
        fs::create_directory(probe, created);
        if (created) {
            fail(StorageErrorCode::Io,
                 "cannot write into '" + parent.string() + "': " + created.message());
        }
    }
    fs::remove(probe, ec);
}

CheckpointReport writeCheckpoint(MetadataStore& metadata, const BlobStore& blobs,
                                 const fs::path& destination) {
    const auto started = std::chrono::steady_clock::now();

    requireUsableDestination(destination);

    CheckpointReport report;
    report.destination = destination;
    report.takenAtMs   = nowMs();

    std::error_code ec;
    fs::create_directories(destination, ec);
    if (ec) {
        fail(StorageErrorCode::Io,
             "could not create '" + destination.string() + "': " + ec.message());
    }

    // RocksDB refuses a destination that already exists, so the metadata
    // directory is named and left to it rather than created here.
    metadata.checkpointTo((destination / "meta").string());

    const fs::path objects = destination / "objects";
    fs::create_directories(objects, ec);
    if (ec) {
        fail(StorageErrorCode::Io,
             "could not create '" + objects.string() + "': " + ec.message());
    }

    blobs.forEachBlob([&](const BlobStore::TreeEntry& entry) {
        // A file whose name is not a blob id cannot be referenced by anything,
        // so it is left behind rather than carried into the copy. --fsck
        // reports those against the original, which is where they can be dealt
        // with.
        if (!entry.wellFormed) return;

        const fs::path source = blobs.pathFor(entry.blobId);
        const fs::path target = objects / fs::relative(source, blobs.root() / "objects", ec);

        std::error_code parentError;
        fs::create_directories(target.parent_path(), parentError);
        if (parentError) {
            fail(StorageErrorCode::Io, "could not create '" + target.parent_path().string() +
                                           "': " + parentError.message());
        }

        std::error_code linkError;
        fs::create_hard_link(source, target, linkError);
        if (!linkError) {
            ++report.payloadsLinked;
            return;
        }

        // EXDEV — a different filesystem, where a hard link cannot exist. The
        // copy is correct and is what makes a checkpoint onto another volume
        // possible at all; it is simply no longer free, and the report says so
        // rather than leaving the operator to infer it from the elapsed time.
        std::error_code copyError;
        fs::copy_file(source, target, fs::copy_options::overwrite_existing, copyError);
        if (copyError) {
            // The source vanishing here is the one failure worth naming
            // separately: it means something unlinked a payload during the
            // pass, which StorageEngine::checkpoint() exists to prevent.
            fail(StorageErrorCode::Io, "could not copy payload " + entry.blobId + ": " +
                                           copyError.message());
        }
        ++report.payloadsCopied;
        report.bytesCopied += entry.size;
    });

    report.elapsedMs = millisSince(started);
    return report;
}

}  // namespace monobucket
