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
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nexora {
    namespace graph {

// forward
        class LiveGraph;

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
            std::string name_;

            // ── ساختار ذخیره‌سازی داخلی ──

            struct NodeSnap {
                TypeId   type_id    = kInvalidTypeId;
                uint64_t out_degree = 0;
                uint64_t in_degree  = 0;
                ExtId    ext_id;
                bool     valid      = false;
            };

            struct EdgeSnap {
                DenseId src     = kInvalidDenseId;
                DenseId dst     = kInvalidDenseId;
                TypeId  type_id = kInvalidTypeId;
            };

            // dense vector — index = DenseId (سریع‌ترین دسترسی)
            std::vector<NodeSnap>  nodes_;

            // edge storage
            std::unordered_map<EdgeId, EdgeSnap> edges_;

            // adjacency lists — برای traversal بدون scan کامل
            std::vector<std::vector<AdjEntry>> out_adj_;
            std::vector<std::vector<AdjEntry>> in_adj_;

            // type name maps
            std::unordered_map<TypeId, std::string> node_type_names_;
            std::unordered_map<TypeId, std::string> edge_type_names_;

            // ExtId → DenseId برای denseId() lookup
            std::unordered_map<ExtId, DenseId> ext_to_dense_;

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
