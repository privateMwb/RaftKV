#pragma once

// A scratch directory under the system temp dir, removed (recursively) when
// the object goes out of scope -- one per test case that touches the disk.

// clang-format off
#include <atomic>      // std::atomic - unique suffix per directory
#include <filesystem>  // std::filesystem::path, create_directories, remove_all
#include <string>      // std::string
#include <unistd.h>    // ::getpid - keeps parallel test runs apart
// clang-format on

class TempDir {
  public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("raftkv-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    /// @return The directory itself.
    [[nodiscard]] std::string str() const {
        return path_.string();
    }

    /// @return `name` inside the directory.
    [[nodiscard]] std::string file(const std::string& name) const {
        return (path_ / name).string();
    }

  private:
    std::filesystem::path path_;
};
