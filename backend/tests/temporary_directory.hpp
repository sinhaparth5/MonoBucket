#pragma once

#include <filesystem>
#include <random>
#include <string>

namespace monobucket::testing {

/// A directory that removes itself. Storage tests need a real filesystem —
/// fsync, rename and statvfs have no meaningful fake — so each one gets its own
/// tree rather than sharing state through a fixed path.
class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const std::string& label) {
        static std::mt19937_64 engine{std::random_device{}()};

        path_ = std::filesystem::temp_directory_path() /
                ("monobucket-" + label + '-' + std::to_string(engine()));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TemporaryDirectory(const TemporaryDirectory&)            = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }
    operator const std::filesystem::path&() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace monobucket::testing
