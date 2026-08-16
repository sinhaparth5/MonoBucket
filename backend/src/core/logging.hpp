#pragma once

#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

// A deliberately small structured-ish logger. It stays independent of Drogon
// so that core and storage code can be unit tested without linking an HTTP
// stack; server.cpp mirrors the level onto Trantor at startup.

namespace monobucket::log {

enum class Level { Trace = 0, Debug, Info, Warn, Error, Off };

std::string_view          toString(Level level);
std::optional<Level>      fromString(std::string_view name);

void  setLevel(Level level) noexcept;
Level level() noexcept;

/// Writes one already-formatted line. Serialised across threads.
void write(Level level, std::string_view message);

namespace detail {
template <Level L, typename... Args>
inline void emit(Args&&... args) {
    if (static_cast<int>(L) < static_cast<int>(level())) return;
    std::ostringstream os;
    (os << ... << std::forward<Args>(args));
    write(L, os.str());
}
}  // namespace detail

template <typename... Args> inline void trace(Args&&... a) { detail::emit<Level::Trace>(std::forward<Args>(a)...); }
template <typename... Args> inline void debug(Args&&... a) { detail::emit<Level::Debug>(std::forward<Args>(a)...); }
template <typename... Args> inline void info(Args&&... a)  { detail::emit<Level::Info>(std::forward<Args>(a)...); }
template <typename... Args> inline void warn(Args&&... a)  { detail::emit<Level::Warn>(std::forward<Args>(a)...); }
template <typename... Args> inline void error(Args&&... a) { detail::emit<Level::Error>(std::forward<Args>(a)...); }

}  // namespace monobucket::log
