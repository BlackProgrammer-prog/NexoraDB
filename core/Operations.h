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
    enum WriteStage { CommitStage, CounterLookupStage, DdlWaitStage, WriteStageCount };
    struct StageCounter { std::atomic<uint64_t> calls{0},nanoseconds{0},max_nanoseconds{0}; };
    std::array<StageCounter,WriteStageCount> write_stages;
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
// Fixed labels, no global mutex or payload allocation on the write path.
struct WriteStageTimer {
    OperationsState::StageCounter* counter;
    std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
    WriteStageTimer(OperationsState& state,OperationsState::WriteStage stage,bool enabled=true)
        :counter(enabled ? &state.write_stages[stage] : nullptr) {}
    ~WriteStageTimer() {
        if(!counter) return;
        const uint64_t ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
        counter->nanoseconds.fetch_add(ns,std::memory_order_relaxed);
        counter->calls.fetch_add(1,std::memory_order_relaxed);
        auto previous=counter->max_nanoseconds.load(std::memory_order_relaxed);
        while(previous<ns && !counter->max_nanoseconds.compare_exchange_weak(previous,ns,std::memory_order_relaxed)) {}
    }
};
}
