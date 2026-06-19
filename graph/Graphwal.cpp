//
// Created by HOME on 6/19/2026.
//

#include "Graphwal.h"
/**
 * @file graph/GraphWAL.cpp
 * @brief پیاده‌سازی کامل GraphWAL
 */

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace nexora {
    namespace graph {

// ══════════════════════════════════════════════════════════════
// §1  Helper
// ══════════════════════════════════════════════════════════════

        int64_t GraphWAL::nowMs() noexcept {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
        }

        std::streamoff GraphWAL::seqToOffset(uint64_t seq) const noexcept {
            return static_cast<std::streamoff>(seq) *
                   static_cast<std::streamoff>(sizeof(WalRecord));
        }

// ══════════════════════════════════════════════════════════════
// §2  Lifecycle
// ══════════════════════════════════════════════════════════════

        GraphWAL::GraphWAL(const std::filesystem::path& wal_path,
                           int64_t retention_secs)
                : wal_path_(wal_path), retention_secs_(retention_secs) {}

        GraphWAL::~GraphWAL() {
            close();
        }

        bool GraphWAL::open() {
            std::lock_guard<std::mutex> lock(mutex_);

            if (std::filesystem::exists(wal_path_)) {
                wal_file_.open(wal_path_,
                               std::ios::in | std::ios::out | std::ios::binary);
            } else {
                wal_file_.open(wal_path_,
                               std::ios::in | std::ios::out | std::ios::binary |
                               std::ios::trunc);
            }

            if (!wal_file_.is_open()) return false;

            // محاسبه تعداد entries موجود و next_seq
            wal_file_.seekg(0, std::ios::end);
            uint64_t file_bytes = static_cast<uint64_t>(wal_file_.tellg());
            total_entries_.store(file_bytes / sizeof(WalRecord));

            // بیشترین seq موجود را پیدا کن
            uint64_t max_seq = 0;
            wal_file_.seekg(0, std::ios::beg);
            WalRecord rec{};
            while (wal_file_.read(reinterpret_cast<char*>(&rec), sizeof(WalRecord))) {
                if (rec.seq > max_seq) max_seq = rec.seq;
            }
            wal_file_.clear();

            next_seq_.store(max_seq + 1);
            is_open_ = true;
            return true;
        }

        void GraphWAL::close() {
            if (!is_open_) return;
            std::lock_guard<std::mutex> lock(mutex_);
            if (wal_file_.is_open()) {
                wal_file_.flush();
                wal_file_.close();
            }
            is_open_ = false;
        }

// ══════════════════════════════════════════════════════════════
// §3  Write Path
// ══════════════════════════════════════════════════════════════

        uint64_t GraphWAL::append(WalOpType op,
                                  uint64_t  node_or_edge_id,
                                  uint64_t  src_id,
                                  uint64_t  dst_id,
                                  uint32_t  type_id,
                                  uint32_t  flags) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!is_open_) return UINT64_MAX;

            uint64_t seq = next_seq_.fetch_add(1);

            WalRecord rec{};
            rec.seq             = seq;
            rec.timestamp_ms    = nowMs();
            rec.op              = op;
            rec.node_or_edge_id = node_or_edge_id;
            rec.src_id          = src_id;
            rec.dst_id          = dst_id;
            rec.type_id         = type_id;
            rec.flags           = flags;
            rec.applied         = false;

            // append به انتهای فایل
            wal_file_.seekp(0, std::ios::end);
            wal_file_.write(reinterpret_cast<const char*>(&rec), sizeof(WalRecord));
            wal_file_.flush();

            total_entries_.fetch_add(1);
            return seq;
        }

        bool GraphWAL::markApplied(uint64_t seq) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!is_open_) return false;

            // جستجوی seq در فایل (sequential scan در نسخه MVP)
            // در production از یک index map استفاده شود
            wal_file_.seekg(0, std::ios::beg);
            WalRecord rec{};
            std::streamoff found_offset = -1;

            while (wal_file_.read(reinterpret_cast<char*>(&rec), sizeof(WalRecord))) {
                if (rec.seq == seq) {
                    found_offset = static_cast<std::streamoff>(wal_file_.tellg()) -
                                   static_cast<std::streamoff>(sizeof(WalRecord));
                    break;
                }
            }
            wal_file_.clear();

            if (found_offset < 0) return false;

            // offset applied در struct: آخرین فیلد قبل از _pad2
            constexpr std::streamoff applied_off =
                    sizeof(uint64_t) +   // seq
                    sizeof(int64_t)  +   // timestamp_ms
                    sizeof(WalOpType)+   // op
                    3                +   // _pad
                    sizeof(uint64_t) +   // node_or_edge_id
                    sizeof(uint64_t) +   // src_id
                    sizeof(uint64_t) +   // dst_id
                    sizeof(uint32_t) +   // type_id
                    sizeof(uint32_t);    // flags

            bool val = true;
            wal_file_.seekp(found_offset + applied_off, std::ios::beg);
            wal_file_.write(reinterpret_cast<const char*>(&val), sizeof(bool));
            wal_file_.flush();

            return wal_file_.good();
        }

// ══════════════════════════════════════════════════════════════
// §4  Recovery Path
// ══════════════════════════════════════════════════════════════

        std::vector<WalRecord> GraphWAL::loadUnapplied() const {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!is_open_) return {};

            std::vector<WalRecord> result;
            wal_file_.seekg(0, std::ios::beg);
            WalRecord rec{};
            while (wal_file_.read(reinterpret_cast<char*>(&rec), sizeof(WalRecord))) {
                if (!rec.applied) {
                    result.push_back(rec);
                }
            }
            wal_file_.clear();

            // مرتب‌سازی بر اساس seq برای replay ترتیبی
            std::sort(result.begin(), result.end(),
                      [](const WalRecord& a, const WalRecord& b) {
                          return a.seq < b.seq;
                      });
            return result;
        }

        size_t GraphWAL::replay(const std::function<bool(const WalRecord&)>& apply_fn) {
            auto pending = loadUnapplied();
            size_t count = 0;
            for (const auto& rec : pending) {
                if (apply_fn(rec)) {
                    markApplied(rec.seq);
                    ++count;
                }
            }
            return count;
        }

// ══════════════════════════════════════════════════════════════
// §5  Maintenance
// ══════════════════════════════════════════════════════════════

        size_t GraphWAL::purgeOld() {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!is_open_) return 0;

            int64_t cutoff_ms = nowMs() - retention_secs_ * 1000LL;

            // جمع‌آوری entries که باید نگه داریم
            std::vector<WalRecord> keep;
            wal_file_.seekg(0, std::ios::beg);
            WalRecord rec{};
            size_t total = 0;

            while (wal_file_.read(reinterpret_cast<char*>(&rec), sizeof(WalRecord))) {
                ++total;
                // نگه دار اگر: جدید است یا هنوز applied نشده
                bool old     = (rec.timestamp_ms < cutoff_ms);
                bool applied = rec.applied;
                if (!old || !applied) {
                    keep.push_back(rec);
                }
            }
            wal_file_.clear();

            size_t removed = total - keep.size();
            if (removed == 0) return 0;

            // بازنویسی فایل
            wal_file_.close();
            wal_file_.open(wal_path_,
                           std::ios::in | std::ios::out | std::ios::binary |
                           std::ios::trunc);
            if (!wal_file_.is_open()) return 0;

            for (const auto& r : keep) {
                wal_file_.write(reinterpret_cast<const char*>(&r), sizeof(WalRecord));
            }
            wal_file_.flush();
            total_entries_.store(keep.size());

            return removed;
        }

        bool GraphWAL::hasPendingEntries() const {
            auto pending = loadUnapplied();
            return !pending.empty();
        }

    } // namespace graph
} // namespace nexora