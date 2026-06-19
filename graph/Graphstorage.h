//
// Created by HOME on 6/19/2026.
//

#ifndef GITIGNORE_GRAPHSTORAGE_H
#define GITIGNORE_GRAPHSTORAGE_H


#pragma once

/**
 * @file graph/GraphStorage.h
 * @brief لایه دیسک GraphEngine — مدیریت فایل‌های .nex / .nexr / .nexl
 *
 * @details
 * این کلاس مسئول:
 *   - نوشتن و خواندن NodeRecord از فایل graph.nex
 *   - نوشتن و خواندن EdgeRecord از فایل graph.nexr
 *   - خواندن/نوشتن GraphMeta از فایل graph_meta.nexl
 *   - rebuild سریع DenseId-map هنگام startup
 *
 * فرمت فایل .nex:
 *   [GraphMeta 512 bytes] — فقط در .nexl
 *   [NodeRecord 32 bytes] × N  — offset = id * 32
 *
 * فرمت فایل .nexr:
 *   [EdgeRecord 32 bytes] × M  — offset = edge_id * 32
 *
 * @note
 * دسترسی به هر رکورد با offset = id * sizeof(Record) → O(1)
 * بدون نیاز به ایندکس جداگانه.
 */

#include "Graphtypes.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace nexora {
    namespace graph {

/**
 * @class GraphStorage
 * @brief مدیریت فایل‌های باینری گراف روی دیسک
 *
 * @note thread-safety: mutex داخلی دارد — هم‌زمان read/write ایمن است.
 */
        class GraphStorage {
        public:
            /**
             * @brief سازنده
             * @param dir    مسیر پوشه graph (مثلاً "./graph/social_graph/")
             * @param name   نام گراف (برای نام‌گذاری فایل‌ها)
             */
            GraphStorage(const std::filesystem::path& dir, const std::string& name);
            ~GraphStorage();

            // non-copyable
            GraphStorage(const GraphStorage&)            = delete;
            GraphStorage& operator=(const GraphStorage&) = delete;

            // ──────────────────────────────────────────────────────────
            // Lifecycle
            // ──────────────────────────────────────────────────────────

            /**
             * @brief فایل‌ها را باز می‌کند یا می‌سازد
             * @return true در صورت موفقیت
             */
            bool open();

            /**
             * @brief فایل‌ها را می‌بندد و flush می‌کند
             */
            void close();

            bool isOpen() const noexcept { return is_open_; }

            // ──────────────────────────────────────────────────────────
            // Meta File (.nexl)
            // ──────────────────────────────────────────────────────────

            /**
             * @brief GraphMeta را از فایل .nexl می‌خواند
             */
            std::optional<GraphMeta> readMeta() const;

            /**
             * @brief GraphMeta را به فایل .nexl می‌نویسد
             */
            bool writeMeta(const GraphMeta& meta);

            /**
             * @brief فقط state را در meta به‌روز می‌کند (بدون نوشتن کل struct)
             */
            bool updateMetaState(GraphState state);

            /**
             * @brief بررسی می‌کند آیا meta clean است یا نیاز به rebuild دارد
             */
            bool isMetaClean() const;

            // ──────────────────────────────────────────────────────────
            // Node File (.nex)
            // ──────────────────────────────────────────────────────────

            /**
             * @brief یک NodeRecord را در offset صحیح می‌نویسد
             * @param rec رکورد (dense_id در آن offset را تعیین می‌کند)
             * @return true در صورت موفقیت
             */
            bool writeNode(const NodeRecord& rec);

            /**
             * @brief یک NodeRecord را با dense_id می‌خواند
             * @return nullopt اگر id خارج از بازه باشد
             */
            std::optional<NodeRecord> readNode(DenseId id) const;

            /**
             * @brief فقط flags یک node را به‌روز می‌کند (atomic-friendly)
             */
            bool updateNodeFlags(DenseId id, uint32_t flags);

            /**
             * @brief فقط degree یک node را به‌روز می‌کند
             */
            bool updateNodeDegree(DenseId id, uint64_t out_degree, uint64_t in_degree);

            /**
             * @brief تعداد رکوردهای کل در فایل .nex (شامل deleted)
             */
            uint64_t nodeFileSize() const noexcept { return node_file_records_; }

            // ──────────────────────────────────────────────────────────
            // Edge File (.nexr)
            // ──────────────────────────────────────────────────────────

            /**
             * @brief یک EdgeRecord را می‌نویسد
             */
            bool writeEdge(const EdgeRecord& rec);

            /**
             * @brief یک EdgeRecord را با edge_id می‌خواند
             */
            std::optional<EdgeRecord> readEdge(EdgeId id) const;

            /**
             * @brief فقط flags یک edge را به‌روز می‌کند
             */
            bool updateEdgeFlags(EdgeId id, uint32_t flags);

            /**
             * @brief تعداد رکوردهای کل در فایل .nexr
             */
            uint64_t edgeFileSize() const noexcept { return edge_file_records_; }

            // ──────────────────────────────────────────────────────────
            // Bulk Scan (برای Startup و Rebuild)
            // ──────────────────────────────────────────────────────────

            /**
             * @brief تمام NodeRecordها را اسکن می‌کند
             * @param callback fn(NodeRecord) → bool (false = توقف)
             *
             * @note این تابع در startup برای بازسازی Free Stack و DenseId-Map استفاده می‌شود
             */
            void scanAllNodes(const std::function<bool(const NodeRecord&)>& callback) const;

            /**
             * @brief تمام EdgeRecordها را اسکن می‌کند
             */
            void scanAllEdges(const std::function<bool(const EdgeRecord&)>& callback) const;

            // ──────────────────────────────────────────────────────────
            // Compaction (پاک‌سازی فضاهای DELETED در زمان خلوت)
            // ──────────────────────────────────────────────────────────

            /**
             * @brief حذف فیزیکی رکوردهای DELETED — shift و rewrite فایل
             * @param id_remap خروجی: mapping از old DenseId به new DenseId
             * @return true در صورت موفقیت
             *
             * @warning این عملیات زمان‌بر است — فقط در زمان خلوت اجرا شود
             * @note بعد از compaction، تمام DenseId‌های RAM باید از id_remap به‌روز شوند
             */
            bool compactNodes(std::unordered_map<DenseId, DenseId>& id_remap);
            bool compactEdges(std::unordered_map<EdgeId, EdgeId>& id_remap);

            // ──────────────────────────────────────────────────────────
            // Utilities
            // ──────────────────────────────────────────────────────────

            /**
             * @brief مسیر فایل .nex
             */
            std::filesystem::path nodePath()  const { return node_path_;  }
            std::filesystem::path edgePath()  const { return edge_path_;  }
            std::filesystem::path metaPath()  const { return meta_path_;  }

            /**
             * @brief sync کردن تمام فایل‌ها به دیسک
             */
            void sync();

        private:
            std::filesystem::path dir_;
            std::string           name_;
            std::filesystem::path node_path_;  ///< graph.nex
            std::filesystem::path edge_path_;  ///< graph.nexr
            std::filesystem::path meta_path_;  ///< graph_meta.nexl

            mutable std::mutex node_mutex_;
            mutable std::mutex edge_mutex_;
            mutable std::mutex meta_mutex_;

            mutable std::fstream node_file_;
            mutable std::fstream edge_file_;
            mutable std::fstream meta_file_;

            bool     is_open_           = false;
            uint64_t node_file_records_ = 0;  ///< تعداد کل رکوردها (cache)
            uint64_t edge_file_records_ = 0;

            bool openFile(std::fstream& f, const std::filesystem::path& path);
            static std::streamoff nodeOffset(DenseId id) noexcept;
            static std::streamoff edgeOffset(EdgeId  id) noexcept;
        };

    } // namespace graph
} // namespace nexora

#endif //GITIGNORE_GRAPHSTORAGE_H
