//
// Created by HOME on 6/19/2026.
//

#ifndef GITIGNORE_ALGORITHMBASE_H
#define GITIGNORE_ALGORITHMBASE_H


#pragma once

/**
 * @file graph/algorithms/AlgorithmBase.h
 * @brief Base interface برای تمام الگوریتم‌های گراف
 *
 * @section تقسیم کار
 * تیم GraphEngine (ما): این فایل را نوشتیم + LiveGraph + GraphStorage
 * تیم الگوریتم: فایل‌های الگوریتم را در همین پوشه اضافه می‌کنند
 *
 * @section دو نوع الگوریتم
 *
 * ┌──────────────────────────────────────────────────────────────┐
 * │  نوع ۱: LockAlgorithm  (الگوریتم‌های سبک، بلادرنگ)          │
 * │  → روی LiveGraph با shared_lock اجرا می‌شود                  │
 * │  → گراف در طول اجرا قابل تغییر نیست (read-only lock)        │
 * │  → مناسب: neighbors, common followers, BFS سطحی             │
 * ├──────────────────────────────────────────────────────────────┤
 * │  نوع ۲: JobAlgorithm  (الگوریتم‌های سنگین، background)      │
 * │  → snapshot از LiveGraph گرفته می‌شود                        │
 * │  → snapshot در background thread پردازش می‌شود               │
 * │  → بعد از پایان، snapshot آزاد می‌شود                        │
 * │  → مناسب: PageRank, community detection, centrality          │
 * └──────────────────────────────────────────────────────────────┘
 */

#include "../Livegraph.h"

#include <future>
#include <string>

namespace nexora {
    namespace graph {
        namespace algorithms {

// ══════════════════════════════════════════════════════════════
// §1  نتیجه الگوریتم
// ══════════════════════════════════════════════════════════════

/**
 * @struct AlgoResult
 * @brief خروجی استاندارد تمام الگوریتم‌ها
 */
            struct AlgoResult {
                bool        success    = false;
                std::string error_msg;
                std::string result_json;    ///< JSON قابل ارسال به Python
                double      elapsed_ms = 0; ///< زمان اجرا (میلی‌ثانیه)
            };

// ══════════════════════════════════════════════════════════════
// §2  StaticGraphView — snapshot برای JobAlgorithm
// ══════════════════════════════════════════════════════════════

/**
 * @class StaticGraphView
 * @brief یک snapshot read-only از LiveGraph برای الگوریتم‌های سنگین
 *
 * @details
 * GraphManager این snapshot را می‌سازد و به JobAlgorithm می‌دهد.
 * بعد از پایان job، snapshot آزاد می‌شود.
 * تیم الگوریتم فقط از متدهای read-only این کلاس استفاده می‌کند.
 */
            class StaticGraphView {
            public:
                explicit StaticGraphView(const LiveGraph& source);
                ~StaticGraphView() = default;

                // ── Read API برای تیم الگوریتم ──

                uint64_t nodeCount() const noexcept { return node_count_; }
                uint64_t edgeCount() const noexcept { return edge_count_; }

                /**
                 * @brief همسایه‌های یک node
                 * @param dense_id   DenseId node شروع
                 * @param direction  Out / In / Both
                 * @param type_id    فیلتر نوع edge (kInvalidTypeId = همه)
                 */
                std::vector<DenseId> neighbors(DenseId    dense_id,
                                               Direction   direction,
                                               TypeId      type_id = kInvalidTypeId) const;

                /**
                 * @brief iterate روی تمام nodes
                 */
                void forEachNode(const std::function<bool(DenseId, TypeId)>& fn) const;

                /**
                 * @brief iterate روی تمام edges
                 */
                void forEachEdge(
                        const std::function<bool(EdgeId, DenseId src, DenseId dst, TypeId)>& fn) const;

                /**
                 * @brief out_degree یک node
                 */
                uint64_t outDegree(DenseId dense_id) const;

                /**
                 * @brief in_degree یک node
                 */
                uint64_t inDegree(DenseId dense_id) const;

                /**
                 * @brief TypeId نوع node
                 */
                TypeId nodeType(DenseId dense_id) const;

                /**
                 * @brief نام نوع از TypeId
                 */
                std::string typeName(TypeId type_id) const;

                /**
                 * @brief ExtId از DenseId
                 */
                ExtId extId(DenseId dense_id) const;

                bool hasNode(DenseId dense_id) const;
                bool hasEdge(DenseId src, DenseId dst, TypeId type_id = kInvalidTypeId) const;

                /**
                 * @brief snapshot این گراف از چه version بود
                 */
                uint64_t snapshotVersion() const noexcept { return snapshot_version_; }

            private:
                // کپی فشرده از گراف (فقط ساختار، بدون properties)
                struct NodeSnap {
                    TypeId   type_id;
                    uint64_t out_degree;
                    uint64_t in_degree;
                    ExtId    ext_id;
                };

                struct EdgeSnap {
                    DenseId src;
                    DenseId dst;
                    TypeId  type_id;
                };

                std::vector<NodeSnap>                           nodes_;
                std::unordered_map<EdgeId, EdgeSnap>            edges_;
                std::unordered_map<DenseId, std::vector<AdjEntry>> out_adj_;
                std::unordered_map<DenseId, std::vector<AdjEntry>> in_adj_;
                std::unordered_map<TypeId, std::string>          type_names_;

                uint64_t node_count_       = 0;
                uint64_t edge_count_       = 0;
                uint64_t snapshot_version_ = 0;
            };

// ══════════════════════════════════════════════════════════════
// §3  LockAlgorithm Base — الگوریتم‌های سبک
// ══════════════════════════════════════════════════════════════

/**
 * @class LockAlgorithm
 * @brief Base class برای الگوریتم‌های سبک که روی LiveGraph اجرا می‌شوند
 *
 * @details
 * - در طول اجرا، shared_lock روی LiveGraph فعال است
 * - گراف تغییر نمی‌کند
 * - اجرا در thread جاری (blocking)
 * - مناسب برای کوئری‌های سریع و بلادرنگ
 *
 * @section مثال استفاده توسط تیم الگوریتم:
 * ```cpp
 * class CommonFollowers : public LockAlgorithm {
 * public:
 *     AlgoResult run(const LiveGraph& graph,
 *                    const std::vector<ExtId>& params) override {
 *         // گراف read-only است در اینجا
 *         DenseId u1 = graph.getDenseId(params[0]);
 *         DenseId u2 = graph.getDenseId(params[1]);
 *         auto f1 = graph.neighbors(u1, Direction::Out, ...);
 *         auto f2 = graph.neighbors(u2, Direction::Out, ...);
 *         // intersection...
 *         return AlgoResult{true, "", result_json};
 *     }
 * };
 * ```
 */
            class LockAlgorithm {
            public:
                virtual ~LockAlgorithm() = default;

                /**
                 * @brief اجرای الگوریتم روی LiveGraph
                 * @param graph   گراف پویا (read-only در طول اجرا)
                 * @param params  پارامترهای ورودی (ExtId ها یا رشته‌های پیکربندی)
                 * @return AlgoResult
                 *
                 * @note
                 * داخل این تابع از graph فقط متدهای const استفاده کنید:
                 *   - graph.neighbors()
                 *   - graph.getDenseId()
                 *   - graph.forEachNode()
                 *   - graph.forEachEdge()
                 *   - graph.hasNode(), graph.hasEdge()
                 *   - graph.getNode(), graph.getEdge()
                 * متدهای mutating (addNode, removeNode, addEdge, ...) ممنوع است.
                 */
                virtual AlgoResult run(const LiveGraph&             graph,
                                       const std::vector<ExtId>&    params) = 0;

                /**
                 * @brief نام الگوریتم (برای لاگ و Python API)
                 */
                virtual std::string name() const = 0;
            };

// ══════════════════════════════════════════════════════════════
// §4  JobAlgorithm Base — الگوریتم‌های سنگین
// ══════════════════════════════════════════════════════════════

/**
 * @class JobAlgorithm
 * @brief Base class برای الگوریتم‌های سنگین با snapshot
 *
 * @details
 * - GraphManager یک snapshot (StaticGraphView) می‌سازد
 * - الگوریتم در یک background thread اجرا می‌شود
 * - بعد از پایان، snapshot آزاد می‌شود
 * - گراف زنده در حین اجرا تغییر می‌کند (snapshot جدا است)
 * - نتیجه از طریق std::future برمی‌گردد
 *
 * @section مثال استفاده توسط تیم الگوریتم:
 * ```cpp
 * class PageRankAlgo : public JobAlgorithm {
 * public:
 *     AlgoResult run(const StaticGraphView& snapshot,
 *                    const std::vector<ExtId>& params) override {
 *         // snapshot جداست از LiveGraph — تغییرات بعدی گراف تأثیر نمی‌گذارند
 *         int max_iter = std::stoi(params[0]);
 *         std::unordered_map<DenseId, double> pr;
 *         snapshot.forEachNode([&](DenseId id, TypeId) {
 *             pr[id] = 1.0 / snapshot.nodeCount();
 *             return true;
 *         });
 *         // ...
 *         return AlgoResult{true, "", result_json};
 *     }
 *     std::string name() const override { return "PageRank"; }
 * };
 * ```
 *
 * @section فراخوانی از Python (Cython):
 * ```python
 * future = engine.submitJob("social_graph", PageRankAlgo(), ["20"])
 * result = future.get()  # blocking یا timeout
 * ```
 */
            class JobAlgorithm {
            public:
                virtual ~JobAlgorithm() = default;

                /**
                 * @brief اجرای الگوریتم روی snapshot ثابت
                 * @param snapshot   read-only view از گراف در لحظه submit
                 * @param params     پارامترهای ورودی
                 * @return AlgoResult
                 *
                 * @note
                 * این تابع در یک background thread اجرا می‌شود.
                 * snapshot بعد از return این تابع آزاد می‌شود.
                 * هیچ وابستگی به LiveGraph یا RocksDB نداشته باشید.
                 */
                virtual AlgoResult run(const StaticGraphView&       snapshot,
                                       const std::vector<ExtId>&    params) = 0;

                virtual std::string name() const = 0;
            };

        } // namespace algorithms
    } // namespace graph
} // namespace nexora


#endif //GITIGNORE_ALGORITHMBASE_H
