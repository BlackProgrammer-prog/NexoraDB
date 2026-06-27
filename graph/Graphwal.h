//
// Created by HOME on 6/19/2026.
//

#ifndef GITIGNORE_GRAPHWAL_H
#define GITIGNORE_GRAPHWAL_H


#pragma once

/**
 * @file graph/GraphWAL.h
 * @brief Write-Ahead Log برای GraphEngine
 *
 * @details
 * WAL تضمین می‌کند که در صورت crash بین نوشتن روی دیسک و اعمال روی RAM،
 * سیستم قابل بازیابی باشد.
 *
 * فرمت فایل graph.wal:
 *   [WalRecord 56 bytes] × N   (append-only)
 *
 * سیاست نگهداری:
 *   - WAL entries قدیمی‌تر از wal_retention_secs به‌صورت دوره‌ای پاک می‌شوند
 *   - هنگام startup، فقط entries اعمال‌نشده (applied=false) replay می‌شوند
 *   - WAL تا ابد رشد نمی‌کند
 *
 * flow:
 *   1. عملیات (addNode/addEdge/...) در WAL ثبت می‌شود
 *   2. روی in-memory graph اعمال می‌شود
 *   3. روی فایل .nex/.nexr نوشته می‌شود
 *   4. entry به‌عنوان applied mark می‌شود
 *   → اگر crash بین ۱ و ۳ رخ دهد: WAL replay تضمین consistency
 */

#include "Graphtypes.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <vector>

namespace nexora {
    namespace graph {

// forward declaration
        class LiveGraph;

/**
 * @class GraphWAL
 * @brief Write-Ahead Log برای گراف
 *
 * @note thread-safety: append و markApplied از چند thread ایمن‌اند
 */
        class GraphWAL {
        public:
            /**
             * @param wal_path       مسیر فایل graph.wal
             * @param retention_secs مدت نگهداری log ها (ثانیه) — پیش‌فرض: 86400 (24h)
             */
            explicit GraphWAL(const std::filesystem::path& wal_path,
                              int64_t retention_secs = 86400);
            ~GraphWAL();

            // non-copyable
            GraphWAL(const GraphWAL&)            = delete;
            GraphWAL& operator=(const GraphWAL&) = delete;

            // ──────────────────────────────────────────────────────────
            // Lifecycle
            // ──────────────────────────────────────────────────────────

            /**
             * @brief فایل WAL را باز می‌کند
             */
            bool open();

            /**
             * @brief فایل را می‌بندد
             */
            void close();

            bool isOpen() const noexcept { return is_open_; }

            // ──────────────────────────────────────────────────────────
            // Write Path
            // ──────────────────────────────────────────────────────────

            /**
             * @brief یک WalRecord را append می‌کند
             * @return شماره ترتیبی اختصاص‌یافته (seq)
             */
            uint64_t append(WalOpType op,
                            uint64_t  node_or_edge_id,
                            uint64_t  src_id   = 0,
                            uint64_t  dst_id   = 0,
                            uint32_t  type_id  = 0,
                            uint32_t  flags    = 0);

            /**
             * @brief یک entry را به‌عنوان applied علامت می‌زند
             * @return true در صورت موفقیت
             */
            bool markApplied(uint64_t seq);

            // ──────────────────────────────────────────────────────────
            // Recovery Path
            // ──────────────────────────────────────────────────────────

            /**
             * @brief تمام entries اعمال‌نشده را بارگذاری می‌کند
             * @return لیست WalRecordهایی که applied=false هستند
             */
            std::vector<WalRecord> loadUnapplied() const;

            /**
             * @brief WAL را روی LiveGraph replay می‌کند
             * @param apply_fn تابعی که هر WalRecord را روی گراف اعمال می‌کند
             * @return تعداد entries replay شده
             */
            size_t replay(const std::function<bool(const WalRecord&)>& apply_fn);

            // ──────────────────────────────────────────────────────────
            // Maintenance
            // ──────────────────────────────────────────────────────────

            /**
             * @brief entries قدیمی‌تر از retention_secs را پاک می‌کند
             * @return تعداد entries پاک‌شده
             *
             * @details
             * WAL نباید تا ابد رشد کند.
             * این متد باید به‌صورت دوره‌ای (مثلاً هر ساعت) فراخوانی شود.
             * فقط entries applied=true و قدیمی‌تر از retention پاک می‌شوند.
             */
            size_t purgeOld();

            /**
             * @brief بررسی سلامت WAL — آیا entries اعمال‌نشده وجود دارد؟
             */
            bool hasPendingEntries() const;

            /**
             * @brief تعداد کل entries در WAL
             */
            uint64_t totalEntries() const noexcept { return total_entries_.load(); }

            /**
             * @brief آخرین seq اختصاص‌یافته
             */
            uint64_t lastSeq() const noexcept { return next_seq_.load() - 1; }

            /**
             * @brief آخرین رکوردهای WAL را از فایل می‌خواند.
             * @param limit حداکثر تعداد رکوردها؛ پیش‌فرض 20
             * @return رکوردها به ترتیب قدیمی‌تر به جدیدتر
             */
            std::vector<WalRecord> loadRecent(size_t limit = 20) const;

        private:
            std::filesystem::path  wal_path_;
            int64_t                retention_secs_;

            mutable std::mutex     mutex_;
            mutable std::fstream   wal_file_;
            bool                   is_open_ = false;

            std::atomic<uint64_t>  next_seq_{0};
            std::atomic<uint64_t>  total_entries_{0};

            /// offset یک entry با seq معین
            std::streamoff seqToOffset(uint64_t seq) const noexcept;

            /// timestamp فعلی به millisecond
            static int64_t nowMs() noexcept;
        };

    } // namespace graph
} // namespace nexora

#endif //GITIGNORE_GRAPHWAL_H
