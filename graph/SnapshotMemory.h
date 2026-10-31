#pragma once
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <limits>
#include <algorithm>
#include <atomic>
#include "core/ResourceBudget.h"

namespace nexora::graph {
// Snapshot-owned allocator requests, including strings and build scratch.
// This is enforceable requested-byte accounting, not an OS RSS limit.
class SnapshotMemory final:public std::pmr::memory_resource {
    std::shared_ptr<nexora::core::ResourceBudget> pool_;
    inline static std::atomic<size_t> process_current_{0},process_peak_{0},process_resources_{0};
    size_t limit_,baseline_,current_=0,peak_=0;
    mutable std::mutex mutex_;
    void* do_allocate(size_t bytes,size_t alignment) override {
        std::lock_guard lock(mutex_);
        if (bytes>limit_-baseline_-current_) throw std::length_error("snapshot allocation budget exceeded");
        if(pool_) pool_->acquire(nexora::core::ResourceBudget::Snapshot,bytes);
        void* result;
        try {result=std::pmr::new_delete_resource()->allocate(bytes,alignment);}
        catch(...) {if(pool_) pool_->release(nexora::core::ResourceBudget::Snapshot,bytes);throw;}
        current_+=bytes; peak_=std::max(peak_,current_);
        const auto total=process_current_.fetch_add(bytes,std::memory_order_relaxed)+bytes;
        auto peak=process_peak_.load(std::memory_order_relaxed);
        while(total>peak && !process_peak_.compare_exchange_weak(peak,total,std::memory_order_relaxed)) {}
        return result;
    }
    void do_deallocate(void* p,size_t bytes,size_t alignment) override {
        std::lock_guard lock(mutex_);
        std::pmr::new_delete_resource()->deallocate(p,bytes,alignment); current_-=bytes;
        if(pool_) pool_->release(nexora::core::ResourceBudget::Snapshot,bytes);
        process_current_.fetch_sub(bytes,std::memory_order_relaxed);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this==&other; }
public:
    explicit SnapshotMemory(size_t limit=0,size_t baseline=0,std::shared_ptr<nexora::core::ResourceBudget> pool={})
        :pool_(std::move(pool)),limit_(limit ? limit : std::numeric_limits<size_t>::max()),baseline_(baseline) {
        if (baseline_>limit_) throw std::length_error("snapshot cache baseline exceeds budget");
        process_resources_.fetch_add(1,std::memory_order_relaxed);
    }
    ~SnapshotMemory() override { process_resources_.fetch_sub(1,std::memory_order_relaxed); }
    // Exclude admission baseline: it belongs to another cache/resource and is not an allocation here.
    size_t ownedRequestedBytes() const { std::lock_guard lock(mutex_);return current_; }
    static size_t processCurrentBytes() { return process_current_.load(std::memory_order_relaxed); }
    static size_t processPeakBytes() { return process_peak_.load(std::memory_order_relaxed); }
    static size_t processResources() { return process_resources_.load(std::memory_order_relaxed); }
    size_t currentBytes() const { std::lock_guard lock(mutex_); return baseline_+current_; }
    size_t peakBytes() const { std::lock_guard lock(mutex_); return baseline_+peak_; }
};
}
