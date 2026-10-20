#pragma once
#include "algorithms/AlgorithmBase.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace nexora::graph {
struct SchedulerOptions {
    size_t workers=2;
    size_t queue_capacity=32;
    size_t max_reserved_bytes=512*1024*1024;
};
struct SchedulerStats { size_t workers=0,queued=0,running=0,reserved_bytes=0; bool stopping=false; };
class JobScheduler {
    using Clock=std::chrono::steady_clock;
    struct Task {
        std::function<algorithms::AlgoResult()> fn;
        std::shared_ptr<std::atomic<bool>> cancel;
        std::promise<algorithms::AlgoResult> promise;
        Clock::time_point submitted,deadline;
        int priority=0;
        size_t reservation=0;
        uint64_t sequence=0;
    };
    SchedulerOptions options_;
    mutable std::mutex mutex_;
    std::mutex shutdown_mutex_;
    std::condition_variable changed_;
    std::vector<std::unique_ptr<Task>> queue_;
    std::vector<std::thread> workers_;
    std::vector<std::shared_ptr<std::atomic<bool>>> running_;
    size_t reserved_=0;
    uint64_t next_sequence_=0;
    bool stopping_=false;
    void work(size_t slot);
public:
    explicit JobScheduler(SchedulerOptions options={});
    ~JobScheduler();
    JobScheduler(const JobScheduler&)=delete;
    std::future<algorithms::AlgoResult> submit(std::function<algorithms::AlgoResult()> fn,
        std::shared_ptr<std::atomic<bool>> cancellation,int priority,size_t reservation,
        std::chrono::milliseconds enqueue_wait,Clock::time_point deadline={});
    SchedulerStats stats() const;
    void shutdown(); // Cooperative cancel, resolve all promises, join all workers.
};
}
