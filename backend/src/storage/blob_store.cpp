#include "storage/blob_store.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <random>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "core/logging.hpp"
#include "storage/metadata_store.hpp"

namespace monobucket {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

[[noreturn]] void failErrno(StorageErrorCode code, const std::string& what) {
    throw StorageError(code, what + ": " + std::strerror(errno));
}

/// Retries around EINTR. Every syscall below is interruptible, and a signal
/// arriving mid-transfer must not truncate an object.
template <typename Fn>
auto retryOnInterrupt(Fn&& fn) -> decltype(fn()) {
    while (true) {
        const auto result = fn();
        if (result >= 0 || errno != EINTR) return result;
    }
}

void fsyncFile(int fd, const std::string& what) {
    if (retryOnInterrupt([&] { return ::fsync(fd); }) != 0) {
        failErrno(StorageErrorCode::Io, "cannot flush " + what);
    }
}

/// Flushing a directory is what makes a rename() durable. Without it the file
/// contents can survive a power cut while the name that reaches them does not.
void fsyncDirectory(const std::filesystem::path& path) {
    const int fd = retryOnInterrupt([&] { return ::open(path.c_str(), O_RDONLY | O_DIRECTORY); });
    if (fd < 0) failErrno(StorageErrorCode::Io, "cannot open directory '" + path.string() + "'");
    const int result = retryOnInterrupt([&] { return ::fsync(fd); });
    const int saved  = errno;
    ::close(fd);
    if (result != 0) {
        errno = saved;
        failErrno(StorageErrorCode::Io, "cannot flush directory '" + path.string() + "'");
    }
}

void createDirectories(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        throw StorageError(StorageErrorCode::Io,
                           "cannot create '" + path.string() + "': " + ec.message());
    }
}

}  // namespace

std::string newBlobId() {
    // A per-thread generator: object ids are minted on every worker, and a
    // shared engine would either need a lock on the hot path or hand out
    // colliding sequences.
    static thread_local std::mt19937_64 engine{std::random_device{}()};

    std::string out;
    out.resize(32);
    for (int half = 0; half < 2; ++half) {
        std::uint64_t value = engine();
        for (int i = 0; i < 16; ++i) {
            out[half * 16 + i] = kHexDigits[value & 0x0F];
            value >>= 4;
        }
    }
    return out;
}

bool isValidBlobId(std::string_view blobId) {
    if (blobId.size() != 32) return false;
    return std::all_of(blobId.begin(), blobId.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

// --- BlobWriter ------------------------------------------------------------

BlobWriter::BlobWriter(const BlobStore& store, std::string blobId, int fd,
                       std::filesystem::path temporaryPath)
    : store_(&store), blobId_(std::move(blobId)), fd_(fd),
      temporaryPath_(std::move(temporaryPath)) {}

BlobWriter::BlobWriter(BlobWriter&& other) noexcept
    : store_(other.store_), blobId_(std::move(other.blobId_)), fd_(other.fd_),
      temporaryPath_(std::move(other.temporaryPath_)), digest_(std::move(other.digest_)),
      committed_(other.committed_) {
    other.fd_        = -1;
    other.committed_ = true;  // the moved-from writer must not clean up
}

BlobWriter& BlobWriter::operator=(BlobWriter&& other) noexcept {
    if (this != &other) {
        abort();
        store_         = other.store_;
        blobId_        = std::move(other.blobId_);
        fd_            = other.fd_;
        temporaryPath_ = std::move(other.temporaryPath_);
        digest_        = std::move(other.digest_);
        committed_     = other.committed_;
        other.fd_        = -1;
        other.committed_ = true;
    }
    return *this;
}

BlobWriter::~BlobWriter() { abort(); }

void BlobWriter::write(std::span<const std::byte> data) {
    if (fd_ < 0) {
        throw StorageError(StorageErrorCode::Internal, "write to a closed blob writer");
    }

    const std::byte* cursor    = data.data();
    std::size_t      remaining = data.size();

    // write() is permitted to transfer fewer bytes than asked, and does so on
    // large buffers and on signals. Looping is not optional.
    while (remaining > 0) {
        const auto written =
            retryOnInterrupt([&] { return ::write(fd_, cursor, remaining); });
        if (written < 0) {
            failErrno(StorageErrorCode::Io, "cannot write payload " + blobId_);
        }
        cursor += written;
        remaining -= static_cast<std::size_t>(written);
    }

    digest_.update(data);
}

void BlobWriter::write(std::string_view data) {
    write(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size()));
}

void BlobWriter::appendBlob(const BlobStore& store, std::string_view blobId) {
    BlobReader source = store.open(blobId);

    // The digest still has to see every byte, so a pure copy_file_range() would
    // save the copy but cost a second pass to hash. Reading into a chunk buffer
    // hashes and writes in the same pass, which is the cheaper trade at the
    // sizes multipart parts actually have.
    std::vector<std::byte> buffer(store_->chunkBytes());
    while (true) {
        const std::size_t read = source.read(buffer.data(), buffer.size());
        if (read == 0) break;
        write(std::span<const std::byte>(buffer.data(), read));
    }
}

BlobWriter::Committed BlobWriter::commit() { return commit(store_->durability()); }

BlobWriter::Committed BlobWriter::commit(Durability durability) {
    if (committed_) {
        throw StorageError(StorageErrorCode::Internal, "blob writer committed twice");
    }
    if (fd_ < 0) {
        throw StorageError(StorageErrorCode::Internal, "commit on a closed blob writer");
    }

    if (durability != Durability::None) {
        fsyncFile(fd_, "payload " + blobId_);
    }

    if (::close(fd_) != 0) {
        const int saved = errno;
        fd_             = -1;
        errno           = saved;
        failErrno(StorageErrorCode::Io, "cannot close payload " + blobId_);
    }
    fd_ = -1;

    const std::filesystem::path finalPath = store_->pathFor(blobId_);
    createDirectories(finalPath.parent_path());

    // rename() within a filesystem is atomic: the payload is either entirely
    // absent or entirely present under its final name, never half-linked.
    if (::rename(temporaryPath_.c_str(), finalPath.c_str()) != 0) {
        failErrno(StorageErrorCode::Io,
                  "cannot publish payload " + blobId_ + " to '" + finalPath.string() + "'");
    }
    committed_ = true;

    if (durability == Durability::Strict) {
        fsyncDirectory(finalPath.parent_path());
    }

    const Digest::Result result = digest_.finish();
    return Committed{blobId_, result.bytes, result.md5, result.sha256};
}

void BlobWriter::abort() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (!committed_ && !temporaryPath_.empty()) {
        std::error_code ec;
        std::filesystem::remove(temporaryPath_, ec);
        committed_ = true;  // idempotent: a second abort does nothing
    }
}

std::uint64_t BlobWriter::written() const noexcept { return digest_.bytes(); }

// --- BlobReader ------------------------------------------------------------

BlobReader::BlobReader(int fd, std::uint64_t size) : fd_(fd), size_(size), end_(size) {}

BlobReader::BlobReader(BlobReader&& other) noexcept
    : fd_(other.fd_), size_(other.size_), offset_(other.offset_), end_(other.end_) {
    other.fd_ = -1;
}

BlobReader& BlobReader::operator=(BlobReader&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        fd_       = other.fd_;
        size_     = other.size_;
        offset_   = other.offset_;
        end_      = other.end_;
        other.fd_ = -1;
    }
    return *this;
}

BlobReader::~BlobReader() {
    if (fd_ >= 0) ::close(fd_);
}

void BlobReader::limitTo(std::uint64_t offset, std::uint64_t length) {
    offset_ = std::min(offset, size_);
    // Clamped rather than rejected: S3 truncates a range that runs past the end
    // of the object instead of failing it.
    end_ = length > size_ - offset_ ? size_ : offset_ + length;
}

std::uint64_t BlobReader::remaining() const noexcept { return end_ > offset_ ? end_ - offset_ : 0; }

std::size_t BlobReader::read(std::byte* out, std::size_t capacity) {
    const std::uint64_t available = remaining();
    if (available == 0 || capacity == 0) return 0;

    const std::size_t wanted =
        static_cast<std::size_t>(std::min<std::uint64_t>(capacity, available));

    // pread rather than read: the descriptor carries no shared file position,
    // so the same payload can be served to several requests concurrently.
    const auto got = retryOnInterrupt(
        [&] { return ::pread(fd_, out, wanted, static_cast<off_t>(offset_)); });
    if (got < 0) failErrno(StorageErrorCode::Io, "cannot read payload");
    if (got == 0) {
        // Short of the recorded size: the file was truncated under us.
        throw StorageError(StorageErrorCode::Corruption,
                           "payload ended " + std::to_string(available) + " bytes early");
    }

    offset_ += static_cast<std::uint64_t>(got);
    return static_cast<std::size_t>(got);
}

// --- BlobStore -------------------------------------------------------------

BlobStore::BlobStore(std::filesystem::path root, Durability durability, std::size_t chunkBytes)
    : root_(std::move(root)), objectsDir_(root_ / "objects"), temporaryDir_(root_ / "tmp"),
      durability_(durability), chunkBytes_(std::max<std::size_t>(chunkBytes, 4096)) {
    createDirectories(objectsDir_);
    createDirectories(temporaryDir_);
}

std::filesystem::path BlobStore::pathFor(std::string_view blobId) const {
    if (!isValidBlobId(blobId)) {
        // A blob id becomes a path component, so an id that did not come from
        // newBlobId() is rejected before it can escape the tree.
        throw StorageError(StorageErrorCode::Corruption,
                           "malformed blob id '" + std::string(blobId) + "'");
    }
    return objectsDir_ / std::string(blobId.substr(0, 2)) / std::string(blobId.substr(2, 2)) /
           std::string(blobId);
}

BlobWriter BlobStore::create() {
    const std::string           blobId       = newBlobId();
    const std::filesystem::path temporary    = temporaryDir_ / (blobId + ".part");

    // O_EXCL: the id is random, so a collision means either a repeat from the
    // generator or a stale file, and silently overwriting either would destroy
    // a payload that something still references.
    const int fd = retryOnInterrupt([&] {
        return ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
    });
    if (fd < 0) {
        failErrno(StorageErrorCode::Io, "cannot open a temporary payload at '" + temporary.string() + "'");
    }

    return BlobWriter(*this, blobId, fd, temporary);
}

BlobReader BlobStore::open(std::string_view blobId) const {
    const std::filesystem::path path = pathFor(blobId);

    const int fd = retryOnInterrupt([&] { return ::open(path.c_str(), O_RDONLY | O_CLOEXEC); });
    if (fd < 0) {
        if (errno == ENOENT) {
            throw StorageError(StorageErrorCode::NoSuchKey,
                               "payload " + std::string(blobId) + " is missing from the store");
        }
        failErrno(StorageErrorCode::Io, "cannot open payload " + std::string(blobId));
    }

    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        const int saved = errno;
        ::close(fd);
        errno = saved;
        failErrno(StorageErrorCode::Io, "cannot stat payload " + std::string(blobId));
    }

    return BlobReader(fd, static_cast<std::uint64_t>(info.st_size));
}

bool BlobStore::exists(std::string_view blobId) const {
    if (!isValidBlobId(blobId)) return false;
    std::error_code ec;
    return std::filesystem::exists(pathFor(blobId), ec) && !ec;
}

bool BlobStore::remove(std::string_view blobId) const {
    if (!isValidBlobId(blobId)) return false;

    std::error_code ec;
    const bool      removed = std::filesystem::remove(pathFor(blobId), ec);
    if (ec) {
        log::warn("cannot reclaim payload ", blobId, ": ", ec.message());
        return false;
    }
    return removed;
}

std::uint64_t BlobStore::sizeOf(std::string_view blobId) const {
    struct stat info {};
    if (::stat(pathFor(blobId).c_str(), &info) != 0) {
        failErrno(StorageErrorCode::Io, "cannot stat payload " + std::string(blobId));
    }
    return static_cast<std::uint64_t>(info.st_size);
}

std::size_t BlobStore::sweepTemporaries() const {
    std::size_t     removed = 0;
    std::error_code ec;

    for (const auto& entry : std::filesystem::directory_iterator(temporaryDir_, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        std::error_code removeError;
        if (std::filesystem::remove(entry.path(), removeError)) {
            ++removed;
        } else if (removeError) {
            log::warn("cannot remove stale temporary '", entry.path().string(),
                      "': ", removeError.message());
        }
    }

    if (ec) log::warn("cannot scan the temporary directory: ", ec.message());
    return removed;
}

std::size_t BlobStore::forEachBlob(const std::function<void(const TreeEntry&)>& visit) const {
    std::size_t     seen = 0;
    std::error_code ec;

    // recursive_directory_iterator with an error_code never throws, so a shard
    // directory that vanishes mid-walk (reclamation runs concurrently) skips
    // that entry instead of aborting the whole scan.
    std::filesystem::recursive_directory_iterator it(objectsDir_, ec);
    if (ec) {
        log::warn("cannot scan the payload tree: ", ec.message());
        return 0;
    }

    for (const auto& entry : it) {
        std::error_code entryError;
        if (!entry.is_regular_file(entryError) || entryError) continue;

        TreeEntry found;
        found.blobId     = entry.path().filename().string();
        found.wellFormed = isValidBlobId(found.blobId);

        struct stat info {};
        if (::stat(entry.path().c_str(), &info) != 0) continue;
        found.size = static_cast<std::uint64_t>(info.st_size);
        found.modifiedMs =
            static_cast<std::int64_t>(info.st_mtim.tv_sec) * 1000 + info.st_mtim.tv_nsec / 1000000;

        ++seen;
        visit(found);
    }
    return seen;
}

BlobStore::SpaceInfo BlobStore::space() const {
    struct statvfs info {};
    if (::statvfs(root_.c_str(), &info) != 0) {
        failErrno(StorageErrorCode::Io, "cannot stat the filesystem at '" + root_.string() + "'");
    }

    // f_frsize is the fragment size, which is what the block counts are in;
    // f_bsize is only a hint about efficient I/O size.
    const auto unit = static_cast<std::uint64_t>(info.f_frsize != 0 ? info.f_frsize : info.f_bsize);

    SpaceInfo space;
    space.totalBytes     = static_cast<std::uint64_t>(info.f_blocks) * unit;
    space.freeBytes      = static_cast<std::uint64_t>(info.f_bfree) * unit;
    space.availableBytes = static_cast<std::uint64_t>(info.f_bavail) * unit;
    return space;
}

}  // namespace monobucket
