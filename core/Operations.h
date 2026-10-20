#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
namespace nexora::core {
// Fixed operation labels and buckets; no document/query values are retained.
struct OperationsState {
    enum Kind { Query, PointRead, Insert, BatchInsert, Update, Delete, Count };
    struct Counter { uint64_t calls=0,errors=0,not_found=0,sum_us=0,max_us=0; std::array<uint64_t,20> buckets{}; };
    mutable std::mutex mutex;
    std::array<Counter,Count> counters;
    std::deque<nlohmann::json> slow_events;
    std::atomic<uint64_t> slow_us{100000};
    uint64_t dropped=0;
    size_t capacity=128;
    bool corruption=false,io_error=false,quiescing=false;
    const std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
};
}
