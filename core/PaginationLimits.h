#pragma once
#include <cstddef>
#include <cstdint>

namespace nexora::core::pagination {
inline constexpr std::uint32_t kMaxRecords = 1000;
inline constexpr std::size_t kMinBytes = 2;
inline constexpr std::size_t kDefaultBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kMaxBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t kMaxSnapshots = 128;
inline constexpr std::int64_t kIdleLeaseSeconds = 120;
}
