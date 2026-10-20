#include "JobScheduler.h"
#include <algorithm>
#include <stdexcept>

namespace nexora::graph {
namespace { thread_local const JobScheduler* executing_scheduler=nullptr; }
JobScheduler::JobScheduler(SchedulerOptions options):options_(options) {
    if (!options.workers || options.workers>256 || !options.queue_capacity || !options.max_reserved_bytes)
        throw std::invalid_argument("scheduler requires 1..256 workers and nonzero queue/memory limits");
    running_.resize(options.workers);
    try { for (size_t i=0;i<options.workers;++i) workers_.emplace_back([this,i] { work(i); }); }
    catch (...) { shutdown(); throw; }
}
JobScheduler::~JobScheduler() { shutdown(); }
std::future<algorithms::AlgoResult> JobScheduler::submit(std::function<algorithms::AlgoResult()> fn,
    std::shared_ptr<std::atomic<bool>> cancel,int priority,size_t reservation,
    std::chrono::milliseconds wait,Clock::time_point deadline) {
    auto task=std::make_unique<Task>(); task->fn=std::move(fn); task->cancel=std::move(cancel);
    if (!task->cancel) task->cancel=std::make_shared<std::atomic<bool>>(false);
    task->priority=priority; task->reservation=reservation; task->deadline=deadline;
    task->submitted=Clock::now(); auto future=task->promise.get_future();
    auto reject=[&](const char* message) { task->promise.set_value({false,message,"",0}); };
    if (executing_scheduler==this) { reject("nested submission to the same scheduler is unsupported"); return future; }
    if (!reservation || reservation>options_.max_reserved_bytes) {
        reject("job resource reservation exceeds scheduler budget"); return future;
    }
    std::unique_lock lock(mutex_);
    const auto available=[&] { return queue_.size()<options_.queue_capacity && reservation<=options_.max_reserved_bytes-reserved_; };
    const auto until=Clock::now()+wait;
    while (!stopping_ && !available() && wait.count()>0 && Clock::now()<until && !task->cancel->load()) {
        if (deadline!=Clock::time_point{} && Clock::now()>=deadline) break;
        changed_.wait_until(lock,std::min(until,Clock::now()+std::chrono::milliseconds(5)));
    }
    if (stopping_) reject("scheduler is shutting down");
    else if (task->cancel->load()) reject("job cancelled before admission");
    else if (deadline!=Clock::time_point{} && Clock::now()>=deadline) reject("job timeout before admission");
    else if (!available()) reject("scheduler overloaded: bounded queue or resource budget exhausted");
    else {
        task->sequence=next_sequence_++;
        // Allocation may throw: only charge after queue insertion succeeds.
        queue_.push_back(std::move(task)); reserved_+=reservation; changed_.notify_all();
    }
    return future;
}
void JobScheduler::work(size_t slot) {
    executing_scheduler=this;
    for (;;) {
        std::unique_ptr<Task> task;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock,[&] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) break;
            const auto best=std::min_element(queue_.begin(),queue_.end(),[](const auto& a,const auto& b) {
                return a->priority!=b->priority ? a->priority>b->priority : a->sequence<b->sequence;
            });
            task=std::move(*best); queue_.erase(best); running_[slot]=task->cancel;
            changed_.notify_all();
        }
        const auto begin=Clock::now(); algorithms::AlgoResult result;
        try {
            if (task->cancel->load()) result={false,"job cancelled","",0};
            else if (task->deadline!=Clock::time_point{} && begin>=task->deadline) result={false,"job timeout in queue","",0};
            else result=task->fn();
            if (task->cancel->load()) result={false,"job cancelled","",0};
            else if (task->deadline!=Clock::time_point{} && Clock::now()>=task->deadline) result={false,"job timeout","",0};
        } catch (const std::exception& error) { result={false,error.what(),"",0}; }
        catch (...) { result={false,"job failed with unknown exception","",0}; }
        result.queue_ms=std::chrono::duration<double,std::milli>(begin-task->submitted).count();
        result.execution_ms=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
        result.reserved_bytes=task->reservation;
        // Destroy captures (including snapshots) BEFORE making reservation reusable.
        task->fn={};
        {
            std::lock_guard lock(mutex_); running_[slot].reset(); reserved_-=task->reservation;
            changed_.notify_all();
        }
        task->promise.set_value(std::move(result));
    }
    executing_scheduler=nullptr;
}
SchedulerStats JobScheduler::stats() const {
    std::lock_guard lock(mutex_);
    return {options_.workers,queue_.size(),size_t(std::count_if(running_.begin(),running_.end(),[](const auto& p) { return bool(p); })),reserved_,stopping_};
}
void JobScheduler::shutdown() {
    if (executing_scheduler==this) throw std::logic_error("scheduler cannot join itself");
    std::lock_guard shutdown_lock(shutdown_mutex_);
    {
        std::lock_guard lock(mutex_); stopping_=true;
        for (auto& task:queue_) task->cancel->store(true);
        for (auto& cancel:running_) if (cancel) cancel->store(true);
        changed_.notify_all();
    }
    for (auto& worker:workers_) if (worker.joinable()) worker.join();
}
}
