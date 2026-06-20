//
// Created by HOME on 6/19/2026.
//

#ifndef GITIGNORE_LIVEGRAPH_H
#define GITIGNORE_LIVEGRAPH_H


#pragma once

/**
 * @file graph/LiveGraph.h
 * @brief گراف پویا در RAM — همواره زنده، تغییر لحظه‌ای
 *
 * @details
 * LiveGraph مسئول:
 *   - نگهداری تمام nodes و edges در RAM
 *   - مپ از ExtId (string _id) به DenseId (uint64 index)
 *   - Adjacency List با Sorted Vector + Chunked Sorted Vector برای heavy nodes
 *   - Free Stack برای reuse رکوردهای حذف‌شده
 *   - CRUD سریع با WAL
 *   - Compaction در زمان خلوت
 *
 * @note
 * این کلاس thread-safe نیست به‌صورت کامل.
 * GraphManager مسئول locking خارجی است.
 * read ops از shared_mutex استفاده می‌کنند.
 */

#include "Graphtypes.h"
#include "Graphstorage.h"
#include "Graphwal.h"

#include <deque>
#include <shared_mutex>
#include <stack>
#include <string>
#include <unordered_map>
#include <vector>

namespace nexora {
    namespace graph {

/**
 * @class DenseIdMap
 * @brief مپ دوطرفه از ExtId به DenseId
 *
 * @details
 * ExtId = string "_id" از RocksDB
 * DenseId = uint64_t index پیوسته برای array access
 */
        class DenseIdMap {
        public:
            /**
             * @brief ExtId را به DenseId مپ می‌کند
             * @return DenseId اختصاص‌یافته (یا موجود)
             */
            DenseId getOrCreate(const ExtId& ext_id, DenseId reuse_id = kInvalidDenseId);

            /**
             * @return DenseId یا kInvalidDenseId
             */
            DenseId get(const ExtId& ext_id) const;

            /**
             * @return ExtId یا ""
             */
            ExtId getExt(DenseId dense_id) const;

            bool hasExt(const ExtId& ext_id) const;
            bool hasDense(DenseId dense_id) const;

            void remove(const ExtId& ext_id);
            void remove(DenseId dense_id);

            size_t size() const noexcept { return ext_to_dense_.size(); }

        private:
            std::unordered_map<ExtId, DenseId>  ext_to_dense_;
            std::unordered_map<DenseId, ExtId>  dense_to_ext_;
            DenseId                             next_dense_ = 0;
        };

/**
 * @class LiveGraph
 * @brief گراف پویا کاملاً در RAM
 *
 * معماری RAM:
 * ```
 * nodes_[DenseId]   → NodeAdj (adjacency list)
 * node_records_[DenseId] → NodeRecord (metadata)
 * edge_records_[EdgeId]  → EdgeRecord
 * id_map_         → DenseIdMap (ExtId ↔ DenseId)
 * node_free_stack_ → DenseIds های آزاد برای reuse
 * edge_free_stack_ → EdgeIds های آزاد برای reuse
 * type_registry_   → TypeRegistry
 * ```
 */
        class LiveGraph {
        public:
            /**
             * @param storage   لایه دیسک (می‌تواند nullptr باشد اگر فقط in-memory باشد)
             * @param wal       WAL (می‌تواند nullptr باشد)
             * @param name      نام گراف
             */
            LiveGraph(GraphStorage* storage, GraphWAL* wal, std::string name);
            ~LiveGraph() = default;

            // non-copyable
            LiveGraph(const LiveGraph&)            = delete;
            LiveGraph& operator=(const LiveGraph&) = delete;

            const std::string& name() const noexcept { return name_; }

            // ──────────────────────────────────────────────────────────
            // §1  Startup / Build
            // ──────────────────────────────────────────────────────────

            /**
             * @brief گراف را از فایل‌های .nex/.nexr روی دیسک بارگذاری می‌کند
             * @details
             * - رکوردهای ACTIVE → وارد RAM می‌شوند
             * - رکوردهای DELETED → وارد Free Stack می‌شوند
             * - WAL replay می‌شود
             *
             * این متد هنگام startup توسط GraphManager فراخوانی می‌شود.
             */
            bool loadFromDisk();

            /**
             * @brief WAL entries اعمال‌نشده را replay می‌کند
             * @return تعداد entries replay شده
             */
            size_t replayWAL();

            // ──────────────────────────────────────────────────────────
            // §2  CRUD — Node
            // ──────────────────────────────────────────────────────────

            /**
             * @brief یک node اضافه می‌کند
             * @param ext_id    شناسه خارجی (از _id سند RocksDB)
             * @param type_name نوع node (مثلاً "User", "Post")
             * @param is_implicit اگر true: توسط edge ساخته شده، نه مستقیم
             * @return DenseId اختصاص‌یافته یا kInvalidDenseId در صورت خطا
             *
             * @details
             * اگر node از قبل وجود داشته باشد، DenseId موجود برمی‌گردد.
             * اگر Free Stack خالی نباشد، DenseId آزاد reuse می‌شود.
             * عملیات در WAL ثبت می‌شود.
             */
            DenseId addNode(const ExtId&      ext_id,
                            const std::string& type_name,
                            bool               is_implicit = false);

            /**
             * @brief یک node را حذف می‌کند (منطقی — وارد Free Stack می‌شود)
             * @return true اگر پیدا و حذف شد
             *
             * @details
             * - node در RAM با FLAG_DELETED علامت‌گذاری می‌شود
             * - تمام edges مرتبط هم حذف می‌شوند
             * - روی دیسک فقط flags آپدیت می‌شود
             * - DenseId به node_free_stack_ اضافه می‌شود
             */
            bool removeNode(const ExtId& ext_id);
            bool removeNode(DenseId dense_id);

            /**
             * @brief آیا node با این ext_id وجود دارد؟
             */
            bool hasNode(const ExtId& ext_id) const;
            bool hasNode(DenseId dense_id) const;

            /**
             * @brief NodeView یک node را برمی‌گرداند
             */
            std::optional<NodeView> getNode(const ExtId& ext_id) const;
            std::optional<NodeView> getNode(DenseId dense_id) const;

            /**
             * @brief DenseId از ExtId
             */
            DenseId getDenseId(const ExtId& ext_id) const;

            /**
             * @brief ExtId از DenseId
             */
            ExtId getExtId(DenseId dense_id) const;

            // ──────────────────────────────────────────────────────────
            // §3  CRUD — Edge
            // ──────────────────────────────────────────────────────────

            /**
             * @brief یک edge اضافه می‌کند
             * @param src_ext    ExtId مبدا
             * @param dst_ext    ExtId مقصد
             * @param type_name  نوع رابطه (مثلاً "FOLLOWS")
             * @param directed   آیا جهت‌دار است؟
             * @return EdgeId اختصاص‌یافته یا kInvalidEdgeId
             *
             * @details
             * اگر src یا dst وجود نداشته باشند، implicit node ساخته می‌شود.
             * اگر edge تکراری باشد، EdgeId موجود برمی‌گردد.
             * از Free Stack برای EdgeId استفاده می‌شود.
             */
            EdgeId addEdge(const ExtId&       src_ext,
                           const ExtId&       dst_ext,
                           const std::string& type_name,
                           bool               directed = true);

            /**
             * @brief یک edge را حذف می‌کند
             * @return true اگر پیدا و حذف شد
             */
            bool removeEdge(EdgeId edge_id);
            bool removeEdge(const ExtId&       src_ext,
                            const ExtId&       dst_ext,
                            const std::string& type_name);

            /**
             * @brief آیا edge وجود دارد؟
             */
            bool hasEdge(const ExtId& src_ext, const ExtId& dst_ext,
                         const std::string& type_name) const;

            /**
             * @brief EdgeView یک edge را برمی‌گرداند
             */
            std::optional<EdgeView> getEdge(EdgeId edge_id) const;

            // ──────────────────────────────────────────────────────────
            // §4  Traversal (برای تیم الگوریتم)
            // ──────────────────────────────────────────────────────────

            /**
             * @brief همسایه‌های یک node را برمی‌گرداند
             * @param dense_id   DenseId node شروع
             * @param direction  Out / In / Both
             * @param type_id    فیلتر نوع edge (kInvalidTypeId = همه)
             * @param limit      حداکثر نتایج (0 = بدون محدودیت)
             * @return لیست DenseId همسایه‌ها
             *
             * @note این API مستقیماً DenseId برمی‌گرداند، نه NodeView،
             *       تا تیم الگوریتم از overhead ساخت NodeView جلوگیری کند.
             */
            std::vector<DenseId> neighbors(DenseId     dense_id,
                                           Direction    direction,
                                           TypeId       type_id = kInvalidTypeId,
                                           size_t       limit   = 0) const;

            /**
             * @brief همسایه‌ها با ExtId
             */
            std::vector<ExtId> neighborsExt(const ExtId& ext_id,
                                            Direction     direction,
                                            const std::string& type_name = "",
                                            size_t        limit = 0) const;

            /**
             * @brief iterate روی تمام nodes فعال
             * @param fn fn(DenseId, NodeRecord) → bool (false=توقف)
             */
            void forEachNode(const std::function<bool(DenseId, const NodeRecord&)>& fn) const;

            /**
             * @brief iterate روی تمام edges فعال
             */
            void forEachEdge(const std::function<bool(EdgeId, const EdgeRecord&)>& fn) const;

            /**
             * @brief iterate روی edges خروجی یک node
             * @param fn fn(AdjEntry) → bool
             */
            void forEachOutEdge(DenseId dense_id,
                                const std::function<bool(const AdjEntry&)>& fn) const;

            void forEachInEdge(DenseId dense_id,
                               const std::function<bool(const AdjEntry&)>& fn) const;

            // ──────────────────────────────────────────────────────────
            // §5  Type Registry API
            // ──────────────────────────────────────────────────────────

            TypeId getOrCreateNodeType(const std::string& name);
            TypeId getOrCreateEdgeType(const std::string& name);
            std::optional<TypeId> getNodeTypeId(const std::string& name) const;
            std::optional<TypeId> getEdgeTypeId(const std::string& name) const;
            std::string getNodeTypeName(TypeId id) const;
            std::string getEdgeTypeName(TypeId id) const;

            // ──────────────────────────────────────────────────────────
            // §6  Stats
            // ──────────────────────────────────────────────────────────

            GraphStats stats() const;

            uint64_t activeNodeCount() const noexcept;
            uint64_t activeEdgeCount() const noexcept;
            uint64_t version() const noexcept { return version_.load(); }

            // ──────────────────────────────────────────────────────────
            // §7  Compaction (در زمان خلوت)
            // ──────────────────────────────────────────────────────────

            /**
             * @brief compaction کامل — حذف رکوردهای DELETED از دیسک
             * @return true در صورت موفقیت
             *
             * @warning در زمان compaction graph باید lock شود
             * @details
             * GraphManager تشخیص می‌دهد سرور در حالت خلوت است
             * و این متد را فراخوانی می‌کند.
             * بعد از compaction، DenseId‌های RAM remapped می‌شوند.
             */
            bool compact();

            // ──────────────────────────────────────────────────────────
            // §8  Clear (برای rebuild)
            // ──────────────────────────────────────────────────────────

            void clear();

        private:
            std::string    name_;
            GraphStorage*  storage_;  ///< owned by GraphManager
            GraphWAL*      wal_;      ///< owned by GraphManager

            // ── RAM Structures ──
            mutable std::shared_mutex       mutex_;

            /// adjacency lists: DenseId → NodeAdj
            std::vector<NodeAdj>            adj_;

            /// node metadata: DenseId → NodeRecord
            std::vector<NodeRecord>         node_records_;

            /// edge metadata: EdgeId → EdgeRecord
            std::unordered_map<EdgeId, EdgeRecord> edge_records_;

            /// ExtId ↔ DenseId
            DenseIdMap                      id_map_;

            /// Type registries
            TypeRegistry                    node_type_reg_;
            TypeRegistry                    edge_type_reg_;

            /// Free Stacks (روی دیسک ذخیره نمی‌شوند — در startup از scan بازسازی می‌شوند)
            std::stack<DenseId>             node_free_stack_;
            std::stack<EdgeId>              edge_free_stack_;

            std::atomic<uint64_t>           next_dense_id_{0};
            std::atomic<uint64_t>           next_edge_id_{0};
            std::atomic<uint64_t>           active_node_count_{0};
            std::atomic<uint64_t>           active_edge_count_{0};
            std::atomic<uint64_t>           version_{0};

            // ── Internal helpers ──

            DenseId allocDenseId();   ///< از Free Stack یا new id
            EdgeId  allocEdgeId();    ///< از Free Stack یا new id

            void ensureCapacity(DenseId id);  ///< resize vectors اگر لازم باشد

            /// اعمال یک WalRecord روی RAM (برای replay)
            bool applyWalRecord(const WalRecord& rec);

            /// دیسک: نوشتن NodeRecord (اگر storage_ وجود دارد)
            void persistNode(const NodeRecord& rec);
            void persistEdge(const EdgeRecord& rec);

            /// حذف edges مرتبط با یک node (برای cascade delete)
            void removeAllEdgesOfNode(DenseId dense_id);
        };

    } // namespace graph
} // namespace nexora

#endif //GITIGNORE_LIVEGRAPH_H
