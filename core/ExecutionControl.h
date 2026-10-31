#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

namespace nexora::core {
class OperationCancelled : public std::runtime_error {
public: using std::runtime_error::runtime_error;
};
// Cooperative checks only. A commit already handed to RocksDB is never cancelled.
class ExecutionControl {
    std::atomic<bool> cancelled_{false};
    std::chrono::steady_clock::time_point deadline_;
public:
    explicit ExecutionControl(uint64_t timeout_ms=30000) {
        if(!timeout_ms || timeout_ms>300000) throw std::invalid_argument("execution timeout must be 1..300000ms");
        deadline_=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout_ms);
    }
    void cancel() noexcept {cancelled_.store(true,std::memory_order_release);}
    void check() const {
        if(cancelled_.load(std::memory_order_acquire)) throw OperationCancelled("execution_cancelled");
        if(std::chrono::steady_clock::now()>=deadline_) throw OperationCancelled("deadline_exceeded");
    }
};
inline thread_local std::shared_ptr<ExecutionControl> current_execution_control;
inline void CheckExecution() {if(current_execution_control) current_execution_control->check();}
class ExecutionGuard {
    std::shared_ptr<ExecutionControl> previous_;
public:
    explicit ExecutionGuard(std::shared_ptr<ExecutionControl> control):previous_(current_execution_control) {
        if(control) control->check();else CheckExecution();
        if(control) current_execution_control=std::move(control);
    }
    ~ExecutionGuard() {current_execution_control=std::move(previous_);}
};
class ExecutionScope {
    std::shared_ptr<ExecutionControl> control_, previous_;
    std::thread::id owner_;
    bool entered_=false;
public:
    explicit ExecutionScope(uint64_t timeout_ms):control_(std::make_shared<ExecutionControl>(timeout_ms)) {}
    ~ExecutionScope() {
        if(entered_ && owner_==std::this_thread::get_id() && current_execution_control==control_)
            current_execution_control=std::move(previous_);
    }
    void cancel() {control_->cancel();}
    void check() const {control_->check();}
    void enter() {
        if(entered_) throw std::logic_error("execution scope already entered");
        control_->check();previous_=current_execution_control;owner_=std::this_thread::get_id();
        current_execution_control=control_;entered_=true;
    }
    void close() {
        if(!entered_) return;
        if(owner_!=std::this_thread::get_id() || current_execution_control!=control_)
            throw std::logic_error("execution scopes must exit on their original thread in nesting order");
        current_execution_control=std::move(previous_);entered_=false;
    }
};
}
