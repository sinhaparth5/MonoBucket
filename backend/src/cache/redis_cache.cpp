#include "cache/redis_cache.hpp"

#if defined(MONOBUCKET_WITH_REDIS)

// A distribution package installs the headers under include/hiredis/; the
// FetchContent build tree leaves them flat beside the sources.
#if defined(MONOBUCKET_HIREDIS_FLAT_HEADERS)
#include <hiredis.h>
#else
#include <hiredis/hiredis.h>
#endif

#include <cstring>
#include <utility>

#include "core/logging.hpp"

namespace monobucket {
namespace {

struct ReplyDeleter {
    void operator()(redisReply* reply) const noexcept {
        if (reply != nullptr) freeReplyObject(reply);
    }
};

using Reply = std::unique_ptr<redisReply, ReplyDeleter>;

timeval toTimeval(std::chrono::milliseconds duration) noexcept {
    timeval tv{};
    tv.tv_sec  = static_cast<time_t>(duration.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((duration.count() % 1000) * 1000);
    return tv;
}

}  // namespace

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

class RedisCache::Connection {
public:
    explicit Connection(const Options& options) : options_(options) {}

    ~Connection() { close(); }

    Connection(const Connection&)            = delete;
    Connection& operator=(const Connection&) = delete;

    /// Runs one command, connecting first if the socket is not open.
    ///
    /// Arguments go over redisCommandArgv rather than a format string: object
    /// keys are arbitrary bytes, and a key containing '%' must not be able to
    /// change the command being sent.
    Reply command(const std::vector<std::string>& args) {
        ensureOpen();

        std::vector<const char*> argv;
        std::vector<std::size_t> lengths;
        argv.reserve(args.size());
        lengths.reserve(args.size());
        for (const auto& arg : args) {
            argv.push_back(arg.data());
            lengths.push_back(arg.size());
        }

        auto* raw = static_cast<redisReply*>(
            redisCommandArgv(ctx_, static_cast<int>(argv.size()), argv.data(), lengths.data()));

        if (raw == nullptr) {
            // A null reply means the transport failed, not the command. The
            // context cannot be reused after that.
            const std::string why = ctx_ != nullptr ? ctx_->errstr : "connection lost";
            close();
            throw CacheError("redis: " + args.front() + " failed: " + why);
        }

        Reply reply(raw);
        if (reply->type == REDIS_REPLY_ERROR) {
            // Server-side refusal (NOAUTH, WRONGTYPE, OOM). The socket is still
            // good, so the connection goes back to the pool.
            throw CacheError("redis: " + args.front() + " refused: " +
                             std::string(reply->str, reply->len));
        }
        return reply;
    }

private:
    void ensureOpen() {
        if (ctx_ != nullptr) return;

        ctx_ = redisConnectWithTimeout(options_.endpoint.host.c_str(),
                                       options_.endpoint.port,
                                       toTimeval(options_.connectTimeout));
        if (ctx_ == nullptr) {
            throw CacheError("redis: out of memory allocating a connection");
        }
        if (ctx_->err != 0) {
            const std::string why = ctx_->errstr;
            close();
            throw CacheError("redis: cannot connect to " + describe(options_.endpoint) + ": " + why);
        }

        // Without this a stalled server holds an I/O thread indefinitely, which
        // is the failure the bounded pool exists to prevent.
        if (redisSetTimeout(ctx_, toTimeval(options_.commandTimeout)) != REDIS_OK) {
            const std::string why = ctx_->errstr;
            close();
            throw CacheError("redis: cannot set the command timeout: " + why);
        }

        authenticate();
        selectDatabase();
    }

    void authenticate() {
        const auto& endpoint = options_.endpoint;
        if (endpoint.password.empty()) return;

        std::vector<std::string> args{"AUTH"};
        if (!endpoint.username.empty()) args.push_back(endpoint.username);
        args.push_back(endpoint.password);

        // Sent through the raw path rather than command(), which would recurse
        // back into ensureOpen() before authentication has finished.
        issue(args);
    }

    void selectDatabase() {
        if (options_.endpoint.db == 0) return;
        issue({"SELECT", std::to_string(options_.endpoint.db)});
    }

    /// command() without the ensureOpen() step, for the handshake itself.
    void issue(const std::vector<std::string>& args) {
        std::vector<const char*> argv;
        std::vector<std::size_t> lengths;
        argv.reserve(args.size());
        lengths.reserve(args.size());
        for (const auto& arg : args) {
            argv.push_back(arg.data());
            lengths.push_back(arg.size());
        }

        auto* raw = static_cast<redisReply*>(
            redisCommandArgv(ctx_, static_cast<int>(argv.size()), argv.data(), lengths.data()));

        if (raw == nullptr) {
            const std::string why = ctx_ != nullptr ? ctx_->errstr : "connection lost";
            close();
            throw CacheError("redis: " + args.front() + " failed: " + why);
        }

        Reply reply(raw);
        if (reply->type == REDIS_REPLY_ERROR) {
            const std::string why(reply->str, reply->len);
            close();
            throw CacheError("redis: " + args.front() + " refused: " + why);
        }
    }

    void close() noexcept {
        if (ctx_ != nullptr) {
            redisFree(ctx_);
            ctx_ = nullptr;
        }
    }

    const Options& options_;
    redisContext*  ctx_ = nullptr;
};

// ---------------------------------------------------------------------------
// Lease
// ---------------------------------------------------------------------------

RedisCache::Lease::Lease(RedisCache& owner, std::unique_ptr<Connection> connection)
    : owner_(owner), connection_(std::move(connection)) {}

RedisCache::Lease::~Lease() {
    // Returned even when the command threw: a connection that closed its own
    // socket is still a valid pool slot and will reconnect on next use.
    owner_.release(std::move(connection_));
}

// ---------------------------------------------------------------------------
// RedisCache
// ---------------------------------------------------------------------------

RedisCache::RedisCache(Options options) : options_(std::move(options)) {
    if (options_.poolSize == 0) options_.poolSize = 1;
    idle_.reserve(options_.poolSize);
}

RedisCache::~RedisCache() = default;

std::string RedisCache::qualify(std::string_view key) const {
    std::string out;
    out.reserve(options_.keyPrefix.size() + key.size());
    out.append(options_.keyPrefix);
    out.append(key);
    return out;
}

RedisCache::Lease RedisCache::acquire() {
    std::unique_lock lock(poolMutex_);

    for (;;) {
        if (!idle_.empty()) {
            auto connection = std::move(idle_.back());
            idle_.pop_back();
            return Lease(*this, std::move(connection));
        }

        if (created_ < options_.poolSize) {
            ++created_;
            return Lease(*this, std::make_unique<Connection>(options_));
        }

        if (poolAvailable_.wait_for(lock, options_.acquireTimeout,
                                    [this] { return !idle_.empty(); })) {
            continue;
        }

        // Shedding load beats queueing behind a Redis that has stopped
        // answering: the breaker upstream reads this as a failure and starts
        // bypassing, which is the outcome we want.
        throw CacheError("redis: no connection available within " +
                         std::to_string(options_.acquireTimeout.count()) + "ms");
    }
}

void RedisCache::release(std::unique_ptr<Connection> connection) {
    {
        std::lock_guard lock(poolMutex_);
        idle_.push_back(std::move(connection));
    }
    poolAvailable_.notify_one();
}

CacheValuePtr RedisCache::get(std::string_view key) {
    try {
        auto  lease = acquire();
        Reply reply = lease->command({"GET", qualify(key)});

        if (reply->type == REDIS_REPLY_NIL) {
            misses_.fetch_add(1, std::memory_order_relaxed);
            healthy_.store(true, std::memory_order_relaxed);
            return nullptr;
        }
        if (reply->type != REDIS_REPLY_STRING) {
            throw CacheError("redis: GET returned an unexpected reply type " +
                             std::to_string(reply->type));
        }

        hits_.fetch_add(1, std::memory_order_relaxed);
        healthy_.store(true, std::memory_order_relaxed);
        return makeCacheValue(std::string(reply->str, reply->len));
    } catch (const CacheError&) {
        errors_.fetch_add(1, std::memory_order_relaxed);
        healthy_.store(false, std::memory_order_relaxed);
        throw;
    }
}

void RedisCache::set(std::string_view key, CacheValuePtr value, std::chrono::seconds ttl) {
    // A null value is a delete; an already-lapsed ttl means the same thing,
    // since SET PX rejects a non-positive expiry outright.
    if (value == nullptr || ttl < kNoExpiry) {
        del(key);
        return;
    }

    std::vector<std::string> args{"SET", qualify(key), *value};
    if (ttl > kNoExpiry) {
        args.emplace_back("PX");
        args.push_back(std::to_string(std::chrono::milliseconds(ttl).count()));
    }

    try {
        auto lease = acquire();
        lease->command(args);
        healthy_.store(true, std::memory_order_relaxed);
    } catch (const CacheError&) {
        errors_.fetch_add(1, std::memory_order_relaxed);
        healthy_.store(false, std::memory_order_relaxed);
        throw;
    }
}

bool RedisCache::del(std::string_view key) {
    try {
        auto  lease = acquire();
        Reply reply = lease->command({"DEL", qualify(key)});
        healthy_.store(true, std::memory_order_relaxed);
        return reply->type == REDIS_REPLY_INTEGER && reply->integer > 0;
    } catch (const CacheError&) {
        errors_.fetch_add(1, std::memory_order_relaxed);
        healthy_.store(false, std::memory_order_relaxed);
        throw;
    }
}

std::size_t RedisCache::evict(std::uint64_t /*budgetBytes*/) {
    return 0;
}

void RedisCache::clear() {
    const std::string pattern = options_.keyPrefix + "*";
    std::string       cursor  = "0";
    std::size_t       removed = 0;

    try {
        // One lease for the whole scan. Cursors are server-side stateless, so
        // spreading it across connections would work — it would just churn the
        // pool for no reason.
        auto lease = acquire();
        do {
            Reply reply =
                lease->command({"SCAN", cursor, "MATCH", pattern, "COUNT", "512"});

            if (reply->type != REDIS_REPLY_ARRAY || reply->elements != 2) {
                throw CacheError("redis: SCAN returned an unexpected reply");
            }

            cursor            = std::string(reply->element[0]->str, reply->element[0]->len);
            const auto* batch = reply->element[1];

            if (batch->elements > 0) {
                std::vector<std::string> args{"DEL"};
                args.reserve(batch->elements + 1);
                for (std::size_t i = 0; i < batch->elements; ++i) {
                    args.emplace_back(batch->element[i]->str, batch->element[i]->len);
                }
                lease->command(args);
                removed += batch->elements;
            }
        } while (cursor != "0");

        healthy_.store(true, std::memory_order_relaxed);
        log::debug("cache: cleared ", removed, " redis keys under '", options_.keyPrefix, "'");
    } catch (const CacheError&) {
        errors_.fetch_add(1, std::memory_order_relaxed);
        healthy_.store(false, std::memory_order_relaxed);
        throw;
    }
}

CacheStats RedisCache::stats() const {
    CacheStats out;
    out.hits   = hits_.load(std::memory_order_relaxed);
    out.misses = misses_.load(std::memory_order_relaxed);
    out.errors = errors_.load(std::memory_order_relaxed);
    // entries and bytes stay 0: the server holds keys from every instance
    // sharing it, so reporting its totals as ours would be a lie.
    out.healthy = healthy_.load(std::memory_order_relaxed);
    return out;
}

std::optional<std::string> RedisCache::probe() {
    try {
        auto lease = acquire();
        lease->command({"PING"});
        healthy_.store(true, std::memory_order_relaxed);
        return std::nullopt;
    } catch (const CacheError& ex) {
        healthy_.store(false, std::memory_order_relaxed);
        return std::string(ex.what());
    }
}

}  // namespace monobucket

#endif  // MONOBUCKET_WITH_REDIS
