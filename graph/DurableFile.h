#pragma once

#include <atomic>
#include <cerrno>
#include <filesystem>
#include <string_view>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace nexora::graph::detail {

// Publish a complete metadata file; never truncate the currently published file.
// A false result after rename is ambiguous: the new file may be visible but its
// directory entry is not guaranteed durable. Callers must not report success.
inline bool AtomicWriteFile(const std::filesystem::path& target, std::string_view bytes) {
#ifdef _WIN32
    (void)target;
    (void)bytes;
    return false; // A Windows durability backend is required before enabling it.
#else
    static std::atomic<unsigned long long> sequence{0};
    std::filesystem::path temporary;
    int fd = -1;
    for (int attempt = 0; attempt < 32 && fd < 0; ++attempt) {
        temporary = target.string() + ".tmp-" + std::to_string(::getpid()) + "-" +
                    std::to_string(sequence.fetch_add(1));
        fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST) return false;
    }
    if (fd < 0) return false;
    std::size_t offset = 0;
    bool success = true;
    while (offset < bytes.size()) {
        const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { success = false; break; }
        offset += static_cast<std::size_t>(written);
    }
    if (success && ::fsync(fd) != 0) success = false;
    if (::close(fd) != 0) success = false;
    if (!success || ::rename(temporary.c_str(), target.c_str()) != 0) {
        ::unlink(temporary.c_str()); // Only the exclusively created temporary file.
        return false;
    }
    const auto parent = target.parent_path().empty() ? std::filesystem::path(".") : target.parent_path();
    const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return false;
    success = ::fsync(directory) == 0;
    if (::close(directory) != 0) success = false;
    return success;
#endif
}

inline bool ValidGraphName(std::string_view name) {
    auto alpha = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    if (name.empty() || name.size() > 128 || !alpha(name.front())) return false;
    for (unsigned char c : name)
        if (!alpha(c) && !(c >= '0' && c <= '9')) return false;
    return true;
}
} // namespace nexora::graph::detail
