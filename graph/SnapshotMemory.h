#pragma once
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <limits>
#include <algorithm>

namespace nexora::graph {
// Snapshot-owned allocator requests, including strings and build scratch.
// This is enforceable requested-byte accounting, not an OS RSS limit.
class SnapshotMemory final:public std::pmr::memory_resource {
    size_t limit_,baseline_,current_=0,peak_=0;
    mutable std::mutex mutex_;
    void* do_allocate(size_t bytes,size_t alignment) override {
        std::lock_guard lock(mutex_);
        if (bytes>limit_-baseline_-current_) throw std::length_error("snapshot allocation budget exceeded");
        auto* result=std::pmr::new_delete_resource()->allocate(bytes,alignment);
        current_+=bytes; peak_=std::max(peak_,current_); return result;
    }
    void do_deallocate(void* p,size_t bytes,size_t alignment) override {
        std::lock_guard lock(mutex_);
        std::pmr::new_delete_resource()->deallocate(p,bytes,alignment); current_-=bytes;
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this==&other; }
public:
    explicit SnapshotMemory(size_t limit=0,size_t baseline=0)
        :limit_(limit ? limit : std::numeric_limits<size_t>::max()),baseline_(baseline) {
        if (baseline_>limit_) throw std::length_error("snapshot cache baseline exceeds budget");
    }
    size_t currentBytes() const { std::lock_guard lock(mutex_); return baseline_+current_; }
    size_t peakBytes() const { std::lock_guard lock(mutex_); return baseline_+peak_; }
};
}
