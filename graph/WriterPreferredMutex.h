#pragma once
#include <condition_variable>
#include <mutex>

namespace nexora::graph {
// SharedMutex-compatible admission gate: once a writer queues, new readers
// cannot overtake it. Not recursive and not FIFO between writers.
class WriterPreferredMutex {
    std::mutex mutex_;
    std::condition_variable changed_;
    size_t readers_=0,waiting_writers_=0;
    bool writer_=false;
public:
    void lock() {
        std::unique_lock lock(mutex_); ++waiting_writers_;
        try { changed_.wait(lock,[&] { return !writer_ && !readers_; }); }
        catch (...) { --waiting_writers_; changed_.notify_all(); throw; }
        --waiting_writers_; writer_=true;
    }
    bool try_lock() {
        std::lock_guard lock(mutex_);
        if (writer_ || readers_) return false;
        writer_=true; return true;
    }
    void unlock() { std::lock_guard lock(mutex_); writer_=false; changed_.notify_all(); }
    void lock_shared() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock,[&] { return !writer_ && !waiting_writers_; }); ++readers_;
    }
    bool try_lock_shared() {
        std::lock_guard lock(mutex_);
        if (writer_ || waiting_writers_) return false;
        ++readers_; return true;
    }
    void unlock_shared() {
        std::lock_guard lock(mutex_); --readers_;
        if (!readers_) changed_.notify_all();
    }
};
}
