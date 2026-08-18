#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "storage/digest.hpp"
#include "storage/durability.hpp"

// Payload storage: a sharded directory tree of immutable files, written
// through fixed-size chunks so that memory per transfer is constant regardless
// of object size.

namespace monobucket {

/// A fresh, unguessable payload identifier: 128 random bits as 32 hex
/// characters. Random rather than content-addressed, because content
/// addressing would mean either reference counting every payload or leaking
/// the ones that happen to share bytes.
std::string newBlobId();

/// True for a well-formed blob id. Used before touching the filesystem, since
/// a blob id read back from metadata becomes part of a path.
bool isValidBlobId(std::string_view blobId);

class BlobStore;

/// Streams one payload into the store.
///
/// The payload is written to a temporary file and only linked into the tree by
/// commit(). Until then it is invisible, and a writer destroyed without
/// committing removes it — so an abandoned upload leaves nothing behind.
class BlobWriter {
public:
    ~BlobWriter();

    BlobWriter(BlobWriter&&) noexcept;
    BlobWriter& operator=(BlobWriter&&) noexcept;
    BlobWriter(const BlobWriter&)            = delete;
    BlobWriter& operator=(const BlobWriter&) = delete;

    /// Appends, hashing as it goes. Throws StorageError on a short or failed
    /// write; a full disk surfaces here rather than at commit.
    void write(std::span<const std::byte> data);
    void write(std::string_view data);

    /// Appends the entire contents of another blob, in chunks of the store's
    /// configured size. This is how CompleteMultipartUpload assembles parts, so
    /// it runs over the whole object with memory bounded by one chunk.
    ///
    /// copy_file_range() would let the kernel move the bytes without them
    /// crossing into user space, but the assembled object still needs its own
    /// ETag and SHA-256, and that would then cost a second pass to read the
    /// data back. Hashing during the copy is the cheaper trade.
    void appendBlob(const BlobStore& store, std::string_view blobId);

    struct Committed {
        std::string   blobId;
        std::uint64_t size = 0;
        std::string   md5;     ///< 32 hex chars — the ETag for a single-part object
        std::string   sha256;  ///< 64 hex chars
    };

    /// Flushes to `durability` and links the payload into the tree. After this
    /// returns the bytes are durable to that level, which is the precondition
    /// for publishing the metadata that references them.
    ///
    /// The level is a parameter rather than a property of the store because a
    /// bucket may override the server's setting, and the bucket is only known
    /// once the payload is already streaming — `create()` cannot know it.
    Committed commit(Durability durability);

    /// Flushes to the store's configured durability.
    Committed commit();

    /// Discards the partial payload. Called by the destructor; safe to repeat.
    void abort() noexcept;

    const std::string& blobId() const noexcept { return blobId_; }
    std::uint64_t      written() const noexcept;

private:
    friend class BlobStore;
    BlobWriter(const BlobStore& store, std::string blobId, int fd,
               std::filesystem::path temporaryPath);

    const BlobStore*      store_ = nullptr;
    std::string           blobId_;
    int                   fd_ = -1;
    std::filesystem::path temporaryPath_;
    Digest                digest_;
    bool                  committed_ = false;
};

/// Reads a payload, optionally a byte range of one.
class BlobReader {
public:
    ~BlobReader();

    BlobReader(BlobReader&&) noexcept;
    BlobReader& operator=(BlobReader&&) noexcept;
    BlobReader(const BlobReader&)            = delete;
    BlobReader& operator=(const BlobReader&) = delete;

    /// Total payload size, independent of any range restriction.
    std::uint64_t size() const noexcept { return size_; }

    /// Restricts subsequent reads to `[offset, offset + length)`, clamped to
    /// the payload. Serves the S3 `Range` header.
    void limitTo(std::uint64_t offset, std::uint64_t length);

    /// Bytes still available within the current range.
    std::uint64_t remaining() const noexcept;

    /// Fills up to `capacity` bytes, returning how many were read. Zero means
    /// the range is exhausted. Never blocks on more than one pread.
    std::size_t read(std::byte* out, std::size_t capacity);

    int descriptor() const noexcept { return fd_; }

private:
    friend class BlobStore;
    BlobReader(int fd, std::uint64_t size);

    int           fd_     = -1;
    std::uint64_t size_   = 0;
    std::uint64_t offset_ = 0;
    std::uint64_t end_    = 0;
};

/// The payload tree.
///
/// Layout is `objects/<aa>/<bb>/<blobId>`, taking the shard directories from
/// the first four hex characters of the id. Two levels of 256 gives 65 536 leaf
/// directories, which keeps a million objects at roughly fifteen entries per
/// directory — well inside what every filesystem handles efficiently, and far
/// from the point where directory lookup degrades.
class BlobStore {
public:
    BlobStore(std::filesystem::path root, Durability durability, std::size_t chunkBytes);

    /// Allocates an id and opens a temporary file for it. The caller is
    /// expected to have recorded the id with the metadata store first, so that
    /// a crash before commit still leaves a reclaimable trace.
    BlobWriter create();

    /// Opens an existing payload. Throws StorageError(NoSuchKey) when the
    /// payload is missing — which, for a blob named by a live object row, means
    /// the tree has been tampered with.
    BlobReader open(std::string_view blobId) const;

    bool exists(std::string_view blobId) const;

    /// Unlinks a payload. Returns false when it was already gone, which is not
    /// an error: reclamation is idempotent by design.
    bool remove(std::string_view blobId) const;

    std::uint64_t sizeOf(std::string_view blobId) const;

    /// Removes every leftover temporary file. Nothing under `tmp/` is ever
    /// referenced by metadata, so anything found there is the residue of an
    /// interrupted write. Returns how many were removed.
    std::size_t sweepTemporaries() const;

    /// One entry of the payload tree, as found on disk rather than as metadata
    /// describes it. `blobId` is the file name, which fsck checks rather than
    /// trusts — a file whose name is not a valid id cannot have been written by
    /// this store.
    struct TreeEntry {
        std::string   blobId;
        std::uint64_t size       = 0;
        std::int64_t  modifiedMs = 0;
        bool          wellFormed = false;
    };

    /// Visits every file under `objects/`, in whatever order the filesystem
    /// yields. Streamed through a callback rather than returned as a vector:
    /// the whole point is to walk a tree that may hold more entries than the
    /// caller wants resident, and fsck only needs one at a time.
    ///
    /// Returns how many entries were visited.
    std::size_t forEachBlob(const std::function<void(const TreeEntry&)>& visit) const;

    struct SpaceInfo {
        std::uint64_t totalBytes     = 0;
        std::uint64_t freeBytes      = 0;
        std::uint64_t availableBytes = 0;  ///< free to an unprivileged user
    };

    /// Filesystem capacity backing the data directory, for the dashboard's
    /// capacity panel and the `/metrics` gauges.
    SpaceInfo space() const;

    std::filesystem::path pathFor(std::string_view blobId) const;

    Durability  durability() const noexcept { return durability_; }
    std::size_t chunkBytes() const noexcept { return chunkBytes_; }

    const std::filesystem::path& root() const noexcept { return root_; }

private:
    std::filesystem::path root_;
    std::filesystem::path objectsDir_;
    std::filesystem::path temporaryDir_;
    Durability            durability_;
    std::size_t           chunkBytes_;
};

}  // namespace monobucket
