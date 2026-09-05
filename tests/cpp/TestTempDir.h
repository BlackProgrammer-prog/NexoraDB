//
// Created by HOME on 8/31/2026.
//

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>


class TestTempDir {
public:
    explicit TestTempDir(const std::string& prefix) {
        static std::atomic<std::uint64_t> sequence{0};

        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();

        path_ = std::filesystem::temp_directory_path() / (
                prefix + "_" + std::to_string(timestamp) + "_" +
                std::to_string(sequence.fetch_add(1))
                );

        std::filesystem::create_directories(path_);
    }

    ~TestTempDir() {
        std::error_code error ;
        std::filesystem::remove_all(path_, error);
    }

    TestTempDir(const TestTempDir&) = delete;
    TestTempDir& operator = (const TestTempDir&) = delete;

    [[nodiscard]]
    const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};