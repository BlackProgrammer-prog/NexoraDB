#pragma once
#include <cstdint>

namespace nexora::query {
// The floating operand must be finite. Never round the integer before comparison.
inline int CompareIntDouble(std::int64_t integer, double floating) noexcept {
    if (floating >= 0x1p63) return -1;
    if (floating < -0x1p63) return 1;
    const auto truncated = static_cast<std::int64_t>(floating);
    if (integer < truncated) return -1;
    if (integer > truncated) return 1;
    const double fraction = floating - static_cast<double>(truncated);
    return fraction > 0 ? -1 : (fraction < 0 ? 1 : 0);
}
} // namespace nexora::query
