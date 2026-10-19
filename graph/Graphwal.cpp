#include "Graphwal.h"
#include "DurableFile.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace nexora::graph {
namespace {
void put(std::string& bytes, size_t at, uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) bytes[at+i] = char(value >> (8*i));
}
uint64_t get(std::string_view bytes, size_t at, size_t width) {
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i)
        value |= uint64_t(static_cast<unsigned char>(bytes[at+i])) << (8*i);
    return value;
}
uint32_t crc(std::string_view bytes) {
    uint32_t value = ~uint32_t{0};
    for (unsigned char c : bytes) {
        value ^= c;
        for (int bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ (0xedb88320U & (0U - (value & 1U)));
    }
    return ~value;
}
bool validOp(uint64_t op) { return (op >= 1 && op <= 4) || (op >= 10 && op <= 12); }
uint64_t segmentFirst(const std::filesystem::path& path) {
    const auto name = path.filename().string();
    if (name.size() < 20) return UINT64_MAX;
    uint64_t value = 0;
    const auto parsed = std::from_chars(name.data()+name.size()-20,name.data()+name.size(),value);
    return parsed.ec == std::errc{} ? value : UINT64_MAX;
}
std::string encode(const WalRecord& rec) {
    std::string bytes(GraphWAL::kRecordBytes, '\0');
    bytes.replace(0, 4, "NXW2");
    put(bytes, 4, bytes.size(), 4); put(bytes, 8, 2, 4);
    put(bytes, 16, rec.seq, 8); put(bytes, 24, uint64_t(rec.timestamp_ms), 8);
    put(bytes, 32, uint8_t(rec.op), 4); put(bytes, 36, rec.flags, 4);
    put(bytes, 40, rec.node_or_edge_id, 8); put(bytes, 48, rec.src_id, 8);
    put(bytes, 56, rec.dst_id, 8); put(bytes, 64, rec.type_id, 4);
    put(bytes, 12, crc(bytes), 4);
    return bytes;
}
bool decode(std::string bytes, WalRecord& rec) {
    if (bytes.size() != GraphWAL::kRecordBytes || bytes.substr(0,4) != "NXW2" ||
        get(bytes,4,4) != bytes.size() || get(bytes,8,4) != 2 ||
        get(bytes,68,4) != 0 || !validOp(get(bytes,32,4))) return false;
    const auto expected = get(bytes,12,4); put(bytes,12,0,4);
    if (crc(bytes) != expected) return false;
    rec = {};
    rec.seq = get(bytes,16,8); rec.timestamp_ms = int64_t(get(bytes,24,8));
    rec.op = static_cast<WalOpType>(get(bytes,32,4)); rec.flags = get(bytes,36,4);
    rec.node_or_edge_id = get(bytes,40,8); rec.src_id = get(bytes,48,8);
    rec.dst_id = get(bytes,56,8); rec.type_id = get(bytes,64,4);
    return rec.seq != 0 && rec.seq != UINT64_MAX;
}
bool writeAll(int fd, std::string_view bytes) {
    size_t at = 0;
    while (at < bytes.size()) {
        const auto n = ::write(fd, bytes.data()+at, bytes.size()-at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        at += size_t(n);
    }
    return true;
}
bool syncDir(const std::filesystem::path& path) {
    const auto parent = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    return ::close(fd) == 0 && ok;
}
} // namespace

int64_t GraphWAL::nowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
}
GraphWAL::GraphWAL(const std::filesystem::path& path, int64_t retention, WalOptions options)
    : wal_path_(path), retention_secs_(retention), options_(options) {}
GraphWAL::~GraphWAL() { close(); }
bool GraphWAL::fail(const std::string& message) { error_ = message; failed_ = true; return false; }
std::string GraphWAL::lastError() const { std::lock_guard lock(mutex_); return error_; }
size_t GraphWAL::memoryBytesEstimate() const {
    std::lock_guard lock(mutex_);
    return sizeof(*this)+records_.size()*sizeof(WalRecord)+segments_.capacity()*sizeof(Segment);
}

bool GraphWAL::open() {
    std::lock_guard lock(mutex_);
    if (is_open_) return true;
    records_.clear(); segments_.clear(); error_.clear(); failed_ = false;
    if (options_.segment_bytes < kRecordBytes || options_.group_delay.count() < 0 ||
        options_.group_delay > std::chrono::milliseconds(1000))
        return fail("invalid WAL segment size or group delay");
    applied_lsn_ = 0; next_seq_ = 1; total_entries_ = 0; durable_lsn_ = 0;
    persisted_applied_lsn_ = 0;
    auto cleanup = [&] {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        if (lock_fd_ >= 0) { ::close(lock_fd_); lock_fd_ = -1; }
        return false;
    };
    lock_fd_ = ::open((wal_path_.string()+".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd_ < 0 || ::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
        fail("WAL already owned or lock unavailable"); return cleanup();
    }
    const auto checkpoint = wal_path_.string()+".checkpoint";
    if (std::filesystem::exists(checkpoint)) {
        std::ifstream file(checkpoint, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(file)), {});
        if (bytes.size() != 24 || bytes.substr(0,4) != "NXC2" || get(bytes,4,4) != 2) {
            fail("invalid WAL checkpoint"); return cleanup();
        }
        const auto expected = get(bytes,16,4); put(bytes,16,0,4);
        if (crc(bytes) != expected || get(bytes,20,4) != 0 || get(bytes,8,8) == UINT64_MAX) {
            fail("WAL checkpoint checksum mismatch"); return cleanup();
        }
        applied_lsn_ = get(bytes,8,8);
        persisted_applied_lsn_ = applied_lsn_;
    }
    const auto parent = wal_path_.parent_path().empty() ? std::filesystem::path(".") : wal_path_.parent_path();
    std::vector<std::filesystem::path> paths, extra;
    if (std::filesystem::exists(wal_path_)) paths.push_back(wal_path_);
    const auto prefix = wal_path_.filename().string()+".segment.";
    for (const auto& item : std::filesystem::directory_iterator(parent)) {
        const auto name = item.path().filename().string();
        if (name.starts_with(prefix) && name.size() == prefix.size()+20 &&
            name.find_first_not_of("0123456789", prefix.size()) == std::string::npos)
            extra.push_back(item.path());
    }
    std::sort(extra.begin(), extra.end()); paths.insert(paths.end(), extra.begin(), extra.end());
    if (paths.size() == 1 && paths.front() == wal_path_ && std::filesystem::file_size(wal_path_) > 0) {
        std::ifstream old(wal_path_, std::ios::binary);
        char magic[4]{}; old.read(magic,4);
        const std::string magic_prefix(magic,static_cast<size_t>(old.gcount()));
        old.clear(); old.seekg(0);
        const bool incomplete_first_magic = magic_prefix.size() < 4 &&
                std::string_view("NXW2").starts_with(magic_prefix);
        if (std::memcmp(magic,"NXW2",4) != 0 && !incomplete_first_magic) {
            if (!options_.migrate_legacy) {
                fail("unknown WAL magic; legacy migration requires explicit opt-in"); return cleanup();
            }
            const auto size = std::filesystem::file_size(wal_path_);
            constexpr size_t legacy_bytes = 60;
            if (size % legacy_bytes != 0 || std::filesystem::exists(checkpoint)) {
                fail("unrecognized or torn legacy WAL"); return cleanup();
            }
            std::string migrated; uint64_t previous = 0, sequence = 0;
            std::string raw(legacy_bytes, '\0');
            while (old.read(raw.data(),raw.size())) {
                WalRecord rec{};
                const auto original_seq = get(raw,0,8);
                rec.timestamp_ms = int64_t(get(raw,8,8));
                rec.op = static_cast<WalOpType>(get(raw,16,1));
                rec.node_or_edge_id = get(raw,20,8); rec.src_id = get(raw,28,8);
                rec.dst_id = get(raw,36,8); rec.type_id = get(raw,44,4); rec.flags = get(raw,48,4);
                if (!validOp(uint8_t(rec.op)) || original_seq <= previous || original_seq == UINT64_MAX) {
                    fail("invalid legacy WAL record"); return cleanup();
                }
                previous = original_seq;
                // Legacy retention could leave holes. Migration starts a new
                // contiguous WAL epoch while preserving redo operation order.
                rec.seq = ++sequence; migrated += encode(rec);
            }
            // Replay every migrated record: legacy applied flags had no fsync barrier.
            if (!detail::AtomicWriteFile(wal_path_, migrated)) {
                fail("legacy WAL migration failed"); return cleanup();
            }
        }
    }
    for (size_t i = 0; i < paths.size(); ++i) {
        Segment segment; segment.path = paths[i];
        if (!scanSegmentLocked(segment, i+1 == paths.size())) return cleanup();
        segments_.push_back(segment);
    }
    uint64_t last = records_.empty() ? applied_lsn_.load() : records_.back().seq;
    if (!records_.empty() && records_.front().seq > applied_lsn_.load()+1) {
        fail("missing WAL prefix after checkpoint"); return cleanup();
    }
    const bool empty_successor = records_.empty() && !segments_.empty() &&
            segments_.back().path != wal_path_ &&
            segments_.back().first == applied_lsn_.load()+1;
    if (records_.empty() && applied_lsn_ != 0 && !empty_successor) {
        fail("checkpoint exists without retained WAL"); return cleanup();
    }
    if (last < applied_lsn_) { fail("checkpoint ahead of retained WAL"); return cleanup(); }
    next_seq_ = last+1; total_entries_ = records_.size(); durable_lsn_ = last;
    if (segments_.empty()) {
        if (!createSegmentLocked()) return cleanup();
    } else {
        fd_ = ::open(segments_.back().path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
        if (fd_ < 0) { fail("cannot open active WAL segment"); return cleanup(); }
    }
    // Surviving an earlier process exit does not itself establish a disk
    // barrier for recovered Async bytes. Establish it before durable redo.
    if (options_.durability != WalDurability::Async && !syncLocked()) return cleanup();
    is_open_ = true;
    return true;
}

bool GraphWAL::scanSegmentLocked(Segment& segment, bool active) {
    std::ifstream file(segment.path, std::ios::binary);
    if (!file) return fail("cannot read WAL segment");
    const auto size = std::filesystem::file_size(segment.path);
    const uint64_t named_first = segment.path == wal_path_ ? 0 :
            segmentFirst(segment.path);
    if (named_first == UINT64_MAX || (segment.path != wal_path_ && !named_first))
        return fail("invalid WAL segment name");
    segment.first = named_first;
    if (!active && size == 0) return fail("empty sealed WAL segment");
    size_t offset = 0;
    while (offset + kRecordBytes <= size) {
        std::string bytes(kRecordBytes,'\0');
        file.read(bytes.data(),bytes.size()); WalRecord rec{};
        if (!file || !decode(bytes,rec)) return fail("WAL record corruption at " + segment.path.string()+":"+std::to_string(offset));
        if (!records_.empty() && rec.seq != records_.back().seq+1)
            return fail("non-contiguous WAL sequence");
        if (segment.records == 0) {
            if (named_first && named_first != rec.seq) return fail("WAL segment name/LSN mismatch");
            segment.first = rec.seq;
        }
        segment.last = rec.seq; segment.newest_ms = std::max(segment.newest_ms,rec.timestamp_ms);
        records_.push_back(rec); ++segment.records; offset += kRecordBytes;
    }
    if (offset != size) {
        if (!active) return fail("incomplete sealed WAL segment");
        const uint64_t complete_lsn = records_.empty() ? 0 : records_.back().seq;
        if (complete_lsn < persisted_applied_lsn_ && named_first != persisted_applied_lsn_+1)
            return fail("torn WAL record already covered by checkpoint");
        // Only incomplete physical tails are repairable; a complete bad CRC fails closed.
        int fd = ::open(segment.path.c_str(),O_WRONLY | O_CLOEXEC);
        if (fd < 0) return fail("cannot repair WAL tail");
        bool ok = ::ftruncate(fd,offset) == 0 && ::fdatasync(fd) == 0;
        ::close(fd);
        if (!ok) return fail("WAL tail repair failed");
    }
    segment.bytes = offset;
    return true;
}

bool GraphWAL::createSegmentLocked() {
    if (fd_ >= 0) {
        if (!syncLocked()) return false;
        ::close(fd_); fd_ = -1;
    }
    Segment segment;
    if (segments_.empty()) segment.path = wal_path_;
    else {
        std::ostringstream name;
        name << wal_path_.string() << ".segment." << std::setfill('0') << std::setw(20) << next_seq_.load();
        segment.path = name.str();
    }
    fd_ = ::open(segment.path.c_str(),O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC,0600);
    if (fd_ < 0) return fail("cannot create WAL segment");
    if (::fsync(fd_) != 0 || !syncDir(segment.path)) return fail("cannot sync WAL segment creation");
    segments_.push_back(segment);
    return true;
}
bool GraphWAL::syncLocked() {
    if (fd_ < 0 || failed_) return false;
    if (::fdatasync(fd_) != 0) return fail("WAL fdatasync failed");
    durable_lsn_ = next_seq_.load()-1; ++sync_count_; return true;
}
bool GraphWAL::checkpointLocked() {
    std::string bytes(24,'\0'); bytes.replace(0,4,"NXC2"); put(bytes,4,2,4);
    put(bytes,8,applied_lsn_,8); put(bytes,16,crc(bytes),4);
    if (!detail::AtomicWriteFile(wal_path_.string()+".checkpoint",bytes))
        return fail("WAL checkpoint publication failed");
    persisted_applied_lsn_ = applied_lsn_;
    return true;
}
void GraphWAL::close() {
    std::lock_guard lock(mutex_);
    // Async applied markers stay volatile: recovery conservatively repeats them.
    if (fd_ >= 0) { if (!failed_) syncLocked(); ::close(fd_); fd_ = -1; }
    if (lock_fd_ >= 0) { ::close(lock_fd_); lock_fd_ = -1; }
    is_open_ = false; group_cv_.notify_all();
}
bool GraphWAL::flush() { std::lock_guard lock(mutex_); return is_open_ && syncLocked(); }
bool GraphWAL::checkpoint(const std::function<bool()>& sync_data) {
    std::lock_guard lock(mutex_);
    if (!is_open_ || failed_ || !syncLocked()) return false;
    if (!sync_data || !sync_data()) return fail("graph data barrier failed");
    return checkpointLocked();
}

uint64_t GraphWAL::append(WalOpType op, uint64_t id, uint64_t src, uint64_t dst, uint32_t type, uint32_t flags) {
    std::unique_lock lock(mutex_);
    const auto seq=appendLocked(op,id,src,dst,type,flags);
    return seq!=UINT64_MAX && finishAppendLocked(lock,seq) ? seq : UINT64_MAX;
}
std::vector<uint64_t> GraphWAL::appendBatch(const std::vector<WalRecord>& input) {
    std::unique_lock lock(mutex_);
    if (!is_open_ || failed_ || input.size()>UINT64_MAX-next_seq_.load()) return {};
    for (const auto& rec:input) if (!validOp(uint8_t(rec.op))) return {};
    std::vector<uint64_t> sequences; sequences.reserve(input.size());
    for (const auto& rec:input) {
        const auto seq=appendLocked(rec.op,rec.node_or_edge_id,rec.src_id,rec.dst_id,rec.type_id,rec.flags);
        if (seq==UINT64_MAX) return {};
        sequences.push_back(seq);
    }
    if (!sequences.empty() && !finishAppendLocked(lock,sequences.back())) return {};
    return sequences;
}
uint64_t GraphWAL::appendLocked(WalOpType op, uint64_t id, uint64_t src, uint64_t dst, uint32_t type, uint32_t flags) {
    if (!is_open_ || failed_ || !validOp(uint8_t(op)) || next_seq_ == UINT64_MAX) return UINT64_MAX;
    if (segments_.back().bytes && segments_.back().bytes+kRecordBytes > options_.segment_bytes)
        if (!createSegmentLocked()) return UINT64_MAX;
    WalRecord rec{}; rec.seq = next_seq_; rec.timestamp_ms = nowMs(); rec.op = op;
    rec.node_or_edge_id = id; rec.src_id = src; rec.dst_id = dst; rec.type_id = type; rec.flags = flags;
    if (!writeAll(fd_,encode(rec))) { fail("WAL append failed"); return UINT64_MAX; }
    auto& segment = segments_.back();
    if (!segment.records) segment.first = rec.seq;
    segment.last = rec.seq; segment.newest_ms = std::max(segment.newest_ms,rec.timestamp_ms);
    segment.bytes += kRecordBytes; ++segment.records;
    records_.push_back(rec); ++next_seq_; ++total_entries_;
    return rec.seq;
}
bool GraphWAL::finishAppendLocked(std::unique_lock<std::mutex>& lock,uint64_t seq) {
    if (options_.durability == WalDurability::Sync) {
        if (!syncLocked()) return false;
    } else if (options_.durability == WalDurability::GroupCommit) {
        if (!group_leader_) {
            group_leader_ = true;
            group_cv_.wait_for(lock, options_.group_delay, [&] { return !is_open_ || failed_; });
            const bool ok = is_open_ && syncLocked();
            group_leader_ = false; group_cv_.notify_all();
            if (!ok) return false;
        } else {
            group_cv_.wait(lock,[&] { return durable_lsn_ >= seq || failed_ || !is_open_; });
            if (failed_ || !is_open_) return false;
        }
    }
    return true;
}
bool GraphWAL::markApplied(uint64_t seq) {
    std::lock_guard lock(mutex_);
    return markAppliedLocked(std::span<const uint64_t>(&seq,1));
}
bool GraphWAL::markAppliedBatch(const std::vector<uint64_t>& sequences) {
    std::lock_guard lock(mutex_);
    return markAppliedLocked(sequences);
}
bool GraphWAL::markAppliedLocked(std::span<const uint64_t> sequences) {
    if (!is_open_ || failed_) return false;
    if (sequences.empty()) return true;
    for (const auto seq:sequences)
        if (records_.empty() || seq < records_.front().seq || seq > records_.back().seq) return false;
    for (const auto seq:sequences) records_[seq-records_.front().seq].applied = true;
    uint64_t candidate = applied_lsn_;
    // Each newly contiguous applied record is visited once, amortized O(1).
    while (candidate < records_.back().seq &&
           records_[candidate+1-records_.front().seq].applied) ++candidate;
    if (candidate == applied_lsn_) return true;
    if (options_.durability != WalDurability::Async && durable_lsn_ < candidate && !syncLocked()) return false;
    applied_lsn_ = candidate;
    return options_.durability == WalDurability::Async || checkpointLocked();
}
std::vector<WalRecord> GraphWAL::loadUnapplied() const {
    std::lock_guard lock(mutex_); std::vector<WalRecord> result;
    if (!is_open_) return result;
    for (const auto& record : records_)
        if (record.seq > applied_lsn_ && !record.applied) result.push_back(record);
    return result;
}
bool GraphWAL::hasPendingEntries() const { return !loadUnapplied().empty(); }
std::vector<WalRecord> GraphWAL::loadRecent(size_t limit) const {
    std::lock_guard lock(mutex_); std::vector<WalRecord> result;
    if (!is_open_) return result;
    auto it = records_.end();
    for (size_t i = 0; i < limit && it != records_.begin(); ++i) --it;
    for (; it != records_.end(); ++it) {
        auto record = *it; record.applied = record.applied || record.seq <= applied_lsn_;
        result.push_back(record);
    }
    return result;
}
size_t GraphWAL::replay(const std::function<bool(const WalRecord&)>& apply) {
    const auto pending = loadUnapplied(); size_t count = 0;
    for (const auto& rec : pending) {
        if (!apply(rec) || !markApplied(rec.seq)) break;
        ++count;
    }
    return count;
}
size_t GraphWAL::purgeOld() {
    std::lock_guard lock(mutex_);
    if (!is_open_ || failed_) return 0;
    const auto cutoff = nowMs() - std::clamp<int64_t>(retention_secs_, -1, INT64_MAX/2000)*1000;
    size_t removed = 0;
    while (segments_.size() > 1) {
        const auto& segment = segments_.front();
        if (segment.last > persisted_applied_lsn_ || segment.newest_ms >= cutoff) break;
        if (::unlink(segment.path.c_str()) != 0 || !syncDir(segment.path)) {
            fail("WAL segment retention failed"); break;
        }
        while (!records_.empty() && records_.front().seq <= segment.last)
            records_.pop_front();
        removed += segment.records; total_entries_ -= segment.records;
        segments_.erase(segments_.begin());
    }
    return removed;
}
} // namespace nexora::graph
