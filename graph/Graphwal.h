#pragma once
#include "Graphtypes.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace nexora::graph {
enum class WalDurability { Sync, GroupCommit, Async };
struct WalOptions {
    WalDurability durability = WalDurability::Sync;
    std::size_t segment_bytes = 4 * 1024 * 1024;
    std::chrono::milliseconds group_delay{2};
    bool migrate_legacy = false;
};
// POSIX WAL v2. markApplied requires data durability before checkpointing.
class GraphWAL {
public:
    explicit GraphWAL(const std::filesystem::path&, int64_t retention_secs = 86400,
                      WalOptions options = {});
    ~GraphWAL();
    GraphWAL(const GraphWAL&) = delete;
    GraphWAL& operator=(const GraphWAL&) = delete;
    bool open();
    void close();
    bool isOpen() const noexcept { return is_open_.load(); }
    uint64_t append(WalOpType, uint64_t id, uint64_t src = 0, uint64_t dst = 0,
                    uint32_t type = 0, uint32_t flags = 0);
    bool markApplied(uint64_t seq);
    // One lock and durability barrier for the batch; not an atomic transaction.
    std::vector<uint64_t> appendBatch(const std::vector<WalRecord>&);
    bool markAppliedBatch(const std::vector<uint64_t>&);
    bool flush();
    // Explicit async checkpoint: caller supplies the data durability barrier.
    bool checkpoint(const std::function<bool()>& sync_data);
    std::vector<WalRecord> loadUnapplied() const;
    std::vector<WalRecord> loadRecent(size_t limit = 20) const;
    size_t replay(const std::function<bool(const WalRecord&)>&);
    size_t purgeOld();
    bool hasPendingEntries() const;
    uint64_t totalEntries() const noexcept { return total_entries_.load(); }
    uint64_t lastSeq() const noexcept { return next_seq_.load() - 1; }
    uint64_t appliedLsn() const noexcept { return applied_lsn_.load(); }
    uint64_t syncCount() const noexcept { return sync_count_.load(); }
    WalDurability durability() const noexcept { return options_.durability; }
    std::string lastError() const;
    static constexpr size_t kRecordBytes = 72;
private:
    struct Segment {
        std::filesystem::path path;
        uint64_t first = 0, last = 0;
        int64_t newest_ms = 0;
        size_t bytes = 0, records = 0;
    };
    std::filesystem::path wal_path_;
    int64_t retention_secs_;
    WalOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable group_cv_;
    std::atomic<bool> is_open_{false};
    std::atomic<uint64_t> next_seq_{1}, total_entries_{0}, applied_lsn_{0}, sync_count_{0};
    // Retained LSNs are contiguous: direct indexing avoids history-sized lookups.
    std::deque<WalRecord> records_;
    std::vector<Segment> segments_;
    int fd_ = -1, lock_fd_ = -1;
    bool group_leader_ = false, failed_ = false;
    uint64_t durable_lsn_ = 0;
    uint64_t persisted_applied_lsn_ = 0;
    std::string error_;
    bool fail(const std::string&);
    bool syncLocked();
    bool checkpointLocked();
    bool markAppliedLocked(std::span<const uint64_t>);
    uint64_t appendLocked(WalOpType,uint64_t,uint64_t,uint64_t,uint32_t,uint32_t);
    bool finishAppendLocked(std::unique_lock<std::mutex>&,uint64_t);
    bool createSegmentLocked();
    bool scanSegmentLocked(Segment&, bool active);
    static int64_t nowMs() noexcept;
};
} // namespace nexora::graph
