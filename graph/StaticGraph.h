//
// Created by HOME on 6/19/2026.
//

#ifndef GITIGNORE_STATICGRAPH_H
#define GITIGNORE_STATICGRAPH_H

#pragma once

/**
 * @file graph/StaticGraph.h
 * @brief گراف ثابت — snapshot read-only از LiveGraph برای الگوریتم‌های سنگین
 *
 * @details
 * StaticGraph یک کپی immutable از LiveGraph است که:
 *   - فقط برای JobAlgorithm ساخته می‌شود
 *   - در background thread پردازش می‌شود
 *   - بعد از پایان job آزاد می‌شود
 *   - thread-safe بدون mutex (چون immutable است)
 *
 * @section چرخه حیات
 * ```
 * GraphManager::createSnapshot("social_graph")
 *   ↓ shared_lock روی LiveGraph گرفته می‌شود
 *   ↓ کپی سریع ساختار adjacency
 *   ↓ shared_lock آزاد می‌شود
 *   → StaticGraph آماده (LiveGraph آزاد است تا تغییر کند)
 *
 * background thread: algo.run(*snapshot, params)
 *   ↓ استفاده از neighbor(), forEachNode(), exportCOO()
 *   ↓ هیچ ارتباطی به LiveGraph یا DocEngine ندارد
 *
 * پایان job: unique_ptr<StaticGraph> آزاد می‌شود → حافظه برمی‌گردد
 * ```
 */

#include "Graphtypes.h"

#include <functional>
#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <span>
#include <mutex>
#include <memory>
#include "SnapshotMemory.h"

namespace nexora {
    namespace graph {

// forward
        class LiveGraph;
        struct SnapshotOptions {
            bool incoming=false;
            bool use_cache=false;
            size_t max_build_bytes=0; // preflight + enforced PMR requested-byte cap, not RSS
            std::shared_ptr<std::atomic<bool>> cancellation;
            int job_priority=0;
            uint32_t enqueue_timeout_ms=0;
            uint32_t timeout_ms=0; // Total deadline: queue + snapshot + algorithm.
            size_t workspace_reservation_bytes=64*1024*1024;
            size_t max_result_bytes=16*1024*1024;
            std::chrono::steady_clock::time_point deadline{};
        };

/**
 * @class StaticGraph
 * @brief snapshot read-only از LiveGraph برای الگوریتم‌های سنگین
 *
 * @note
 * این کلاس از GraphStatsEx برای stats() استفاده می‌کند (نه GraphStats)
 * چون فیلدهای nodeCount / edgeCount / nodeTypeCount / edgeTypeCount دارد.
 */
        class StaticGraph {
        public:
            // Logical edges, not duplicated outgoing/incoming traversal arcs.
            std::optional<std::array<uint64_t,5>> edgeAt(size_t offset) const;
            StaticGraph(const LiveGraph&,const std::vector<std::string>&,
                        const std::vector<std::string>&,std::string,SnapshotOptions);
            void checkCancelled() const;
            void cancel() const;
            bool hasIncomingIndex() const;
            size_t plannedPeakBytes() const noexcept { return planned_peak_bytes_; }
            size_t compressedBytes() const;
            size_t allocationPeakBytes() const { return data_->memory.peakBytes(); }
            size_t allocationCurrentBytes() const { return data_->memory.currentBytes(); }
            std::unique_ptr<StaticGraph> clone(SnapshotOptions options={}) const;
            bool cacheHit() const noexcept { return cache_hit_; }
            DenseId originalId(DenseId id) const { return data_->original_ids_.at(id); }
            std::span<const AdjEntry> outgoing(DenseId id) const;
            std::span<const AdjEntry> incoming(DenseId id) const;
            // ──────────────────────────────────────────────────────────
            // سازنده‌ها
            // ──────────────────────────────────────────────────────────

            /**
             * @brief snapshot کامل از LiveGraph (همه nodes و edges)
             * @param source  LiveGraph زنده
             * @param name    نام (برای log و debug)
             */
            explicit StaticGraph(const LiveGraph& source, std::string name = "");

            /**
             * @brief snapshot با فیلتر بر اساس node/edge type
             * @param source      LiveGraph زنده
             * @param node_types  فقط این node types (خالی = همه)
             * @param edge_types  فقط این edge types (خالی = همه)
             * @param name        نام
             */
            StaticGraph(const LiveGraph&                source,
                        const std::vector<std::string>& node_types,
                        const std::vector<std::string>& edge_types,
                        std::string                     name = "");

            ~StaticGraph() = default;

            // non-copyable, movable
            StaticGraph(const StaticGraph&)            = delete;
            StaticGraph& operator=(const StaticGraph&) = delete;
            StaticGraph(StaticGraph&&)                 = default;
            StaticGraph& operator=(StaticGraph&&)      = default;

            // ──────────────────────────────────────────────────────────
            // §A  اطلاعات کلی
            // ──────────────────────────────────────────────────────────

            const std::string& name()           const noexcept { return name_; }
            uint64_t           nodeCount()      const noexcept { return node_count_; }
            uint64_t           edgeCount()      const noexcept { return edge_count_; }
            uint64_t           snapshotVersion()const noexcept { return snapshot_version_; }
            bool               isEmpty()        const noexcept { return node_count_ == 0; }

            // ──────────────────────────────────────────────────────────
            // §B  Node API
            // ──────────────────────────────────────────────────────────

            /** @brief آیا node با این DenseId وجود دارد؟ */
            bool     hasNode(DenseId id)     const noexcept;

            /** @brief TypeId نوع node */
            TypeId   nodeType(DenseId id)    const noexcept;

            /** @brief تعداد یال‌های خروجی */
            uint64_t outDegree(DenseId id)   const noexcept;

            /** @brief تعداد یال‌های ورودی */
            uint64_t inDegree(DenseId id)    const noexcept;

            /** @brief ExtId از DenseId */
            ExtId    extId(DenseId id)       const;

            /** @brief DenseId از ExtId */
            DenseId  denseId(const ExtId& ext) const;

            /** @brief نام نوع node از TypeId */
            std::string nodeTypeName(TypeId tid) const;

            /** @brief نام نوع edge از TypeId */
            std::string edgeTypeName(TypeId tid) const;

            // ──────────────────────────────────────────────────────────
            // §C  Traversal — برای تیم الگوریتم
            // ──────────────────────────────────────────────────────────

            /**
             * @brief همسایه‌های یک node
             * @param id        DenseId
             * @param direction Out / In / Both
             * @param type_id   فیلتر edge type (kInvalidTypeId = همه)
             */
            std::vector<DenseId> neighbors(DenseId   id,
                                           Direction  direction,
                                           TypeId     type_id = kInvalidTypeId) const;

            /**
             * @brief iterate روی همسایه‌ها بدون allocation (سریع‌ترین روش)
             * @param fn fn(neighbor_id, edge_type_id) → bool  (false = توقف)
             */
            void forEachNeighbor(DenseId   id,
                                 Direction  direction,
                                 const std::function<bool(DenseId, TypeId)>& fn) const;

            /**
             * @brief iterate روی تمام nodes فعال
             * @param fn fn(DenseId, TypeId) → bool
             */
            void forEachNode(const std::function<bool(DenseId, TypeId)>& fn) const;

            /**
             * @brief iterate روی تمام edges
             * @param fn fn(EdgeId, DenseId src, DenseId dst, TypeId) → bool
             */
            void forEachEdge(
                    const std::function<bool(EdgeId, DenseId, DenseId, TypeId)>& fn) const;

            /**
             * @brief آیا edge بین src و dst وجود دارد؟
             */
            bool hasEdge(DenseId src, DenseId dst,
                         TypeId type_id = kInvalidTypeId) const;

            // ──────────────────────────────────────────────────────────
            // §D  Export به COO/CSR — برای GPU/ML
            // ──────────────────────────────────────────────────────────

            /**
             * @brief export به COO format (مناسب PyTorch Geometric، DGL)
             * @param opts گزینه‌های export (فیلتر type، remap، ...)
             * @return CooGraph با src[], dst[], edgeTypeIds[]
             */
            CooGraph exportCOO(const GraphExportOptions& opts = {}) const;

            /**
             * @brief export به CSR format (مناسب GPU BFS، cuGraph)
             * @param opts گزینه‌های export
             * @return CsrGraph با rowPtr[], colIdx[]
             */
            CsrGraph exportCSR(const GraphExportOptions& opts = {}) const;

            // ──────────────────────────────────────────────────────────
            // §E  آمار — برای تیم الگوریتم
            // ──────────────────────────────────────────────────────────

            /**
             * @brief آمار کامل snapshot
             * @note از GraphStatsEx استفاده می‌کند (نه GraphStats)
             *       چون فیلدهای nodeCount/edgeCount/nodeTypeCount/edgeTypeCount دارد
             */
            GraphStatsEx stats() const;

            /**
             * @brief تعداد nodes بر اساس type
             */
            std::unordered_map<std::string, size_t> nodeCountByType() const;

            /**
             * @brief تعداد edges بر اساس type
             */
            std::unordered_map<std::string, size_t> edgeCountByType() const;

        private:
            friend class GraphManager;
            StaticGraph(const StaticGraph&,SnapshotOptions);
            bool cache_hit_=false;
            std::string name_;

            // ── ساختار ذخیره‌سازی داخلی ──

            struct NodeSnap {
                TypeId   type_id    = kInvalidTypeId;
                uint64_t out_degree = 0;
                uint64_t in_degree  = 0;
                std::pmr::string ext_id;
                bool     valid      = false;
                NodeSnap(TypeId type,std::string_view ext,std::pmr::memory_resource* resource)
                    :type_id(type),ext_id(ext,resource),valid(true) {}
            };

            struct EdgeSnap {
                DenseId src     = kInvalidDenseId;
                DenseId dst     = kInvalidDenseId;
                TypeId  type_id = kInvalidTypeId;
                bool undirected=false;
            };

            struct StringHash {
                using is_transparent=void;
                size_t operator()(std::string_view value) const noexcept { return std::hash<std::string_view>{}(value); }
            };
            struct StringEqual {
                using is_transparent=void;
                bool operator()(std::string_view a,std::string_view b) const noexcept { return a==b; }
            };
            struct Data {
                SnapshotMemory memory; // Declared first, destroyed after every PMR container.
                std::pmr::vector<NodeSnap> nodes_{&memory};
                std::pmr::vector<std::pair<EdgeId,EdgeSnap>> edges_{&memory};
                std::pmr::vector<uint64_t> out_rows_{&memory},in_rows_{&memory};
                std::pmr::vector<AdjEntry> out_entries_{&memory},in_entries_{&memory};
                std::pmr::vector<DenseId> original_ids_{&memory};
                std::pmr::unordered_map<TypeId,std::pmr::string> node_type_names_{&memory},edge_type_names_{&memory};
                std::pmr::unordered_map<std::pmr::string,DenseId,StringHash,StringEqual> ext_to_dense_{&memory};
                explicit Data(size_t limit,size_t baseline=0):memory(limit,baseline) {}
            };
            std::unique_ptr<Data> data_;
            SnapshotOptions options_;
            size_t planned_peak_bytes_=0;
            mutable std::shared_ptr<std::mutex> incoming_mutex_=std::make_shared<std::mutex>();
            void ensureIncoming() const;


            uint64_t node_count_        = 0;
            uint64_t edge_count_        = 0;
            uint64_t snapshot_version_  = 0;

            // internal builder
            void buildFromLiveGraph(const LiveGraph&                source,
                                    const std::vector<std::string>& node_types,
                                    const std::vector<std::string>& edge_types);
        };

    } // namespace graph
} // namespace nexora
#endif //GITIGNORE_STATICGRAPH_H
