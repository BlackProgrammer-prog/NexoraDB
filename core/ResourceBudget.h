#pragma once
#include <array>
#include <condition_variable>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <cstdint>
#include <atomic>
#include <nlohmann/json.hpp>

namespace nexora::core {
class ResourceExhausted : public std::length_error {
public: explicit ResourceExhausted(const std::string& message):std::length_error(message) {}
};
// Admission credits, not measured RSS. One pool may be shared by engines and Python operators.
class ResourceBudget : public std::enable_shared_from_this<ResourceBudget> {
    inline static std::atomic<uint64_t> next_id_{0};
    const uint64_t id_=++next_id_;
public:
    enum Component { Query, Buffer, Snapshot, Graph, Workspace, Count };
    static constexpr const char* names[Count]={"query","buffer","snapshot","graph","workspace"};
    explicit ResourceBudget(size_t capacity):capacity_(capacity) {
        if(!capacity) throw std::invalid_argument("execution budget must be nonzero");
    }
    void acquire(Component part,size_t bytes,uint32_t wait_ms=0) {
        if(wait_ms>60000) throw std::invalid_argument("resource wait must be <=60000ms");
        std::unique_lock lock(mutex_);
        if(bytes>capacity_) { ++rejected_;throw ResourceExhausted("resource_exhausted: request exceeds shared execution capacity"); }
        const auto available=[&]{return bytes<=capacity_-used_;};
        if(!available() && wait_ms) {++waits_;changed_.wait_for(lock,std::chrono::milliseconds(wait_ms),available);}
        if(!available()) {++rejected_;throw ResourceExhausted("resource_exhausted: shared execution budget exhausted");}
        used_+=bytes;components_[part]+=bytes;peak_=std::max(peak_,used_);
    }
    void release(Component part,size_t bytes) noexcept {
        {std::lock_guard lock(mutex_);used_-=bytes;components_[part]-=bytes;}
        changed_.notify_all();
    }
    std::string report() const {
        std::lock_guard lock(mutex_);nlohmann::json parts=nlohmann::json::object();
        for(size_t i=0;i<Count;++i) parts[names[i]]=components_[i];
        return nlohmann::json{{"pool_id",id_},{"capacity_bytes",capacity_},{"reserved_bytes",used_},{"peak_reserved_bytes",peak_},
            {"components",parts},{"rejected_total",rejected_},{"waits_total",waits_},
            {"scope","shared admission credits; excludes cache/memtable, allocator/Python overhead and charged page cache; not a hard RSS limit"}}.dump();
    }
    static std::shared_ptr<ResourceBudget> processDefault() {
        static auto pool=std::make_shared<ResourceBudget>(size_t(1024)*1024*1024);return pool;
    }
private:
    const size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    size_t used_=0,peak_=0,rejected_=0,waits_=0;
    std::array<size_t,Count> components_{};
};
class BudgetLease {
    std::shared_ptr<ResourceBudget> pool_;
    ResourceBudget::Component component_;
    size_t bytes_=0;
public:
    BudgetLease(std::shared_ptr<ResourceBudget> pool,ResourceBudget::Component component,size_t bytes=0,uint32_t wait_ms=0)
        :pool_(std::move(pool)),component_(component) { resize(bytes,wait_ms); }
    ~BudgetLease() { if(pool_) pool_->release(component_,bytes_); }
    BudgetLease(const BudgetLease&)=delete;
    BudgetLease& operator=(const BudgetLease&)=delete;
    void resize(size_t bytes,uint32_t wait_ms=0) {
        if(!pool_) return;
        if(bytes>bytes_) pool_->acquire(component_,bytes-bytes_,wait_ms);
        else pool_->release(component_,bytes_-bytes);
        bytes_=bytes;
    }
    size_t bytes() const {return bytes_;}
    std::shared_ptr<ResourceBudget> pool() const {return pool_;}
    void close() {resize(0);}
};
} // namespace
