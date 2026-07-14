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
#include "../StaticGraph.h"

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
// §2  LockAlgorithm Base — الگوریتم‌های سبک
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
// §3  JobAlgorithm Base — الگوریتم‌های سنگین
// ══════════════════════════════════════════════════════════════

/**
 * @class JobAlgorithm
 * @brief Base class برای الگوریتم‌های سنگین با snapshot
 *
 * @details
 * - GraphManager یک snapshot (StaticGraph) می‌سازد
 * - الگوریتم در یک background thread اجرا می‌شود
 * - بعد از پایان، snapshot آزاد می‌شود
 * - گراف زنده در حین اجرا تغییر می‌کند (snapshot جدا است)
 * - نتیجه از طریق std::future برمی‌گردد
 *
 * @section مثال استفاده توسط تیم الگوریتم:
 * ```cpp
 * class PageRankAlgo : public JobAlgorithm {
 * public:
 *     AlgoResult run(const StaticGraph& snapshot,
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
                virtual AlgoResult run(const StaticGraph&           snapshot,
                                       const std::vector<ExtId>&    params) = 0;

                virtual std::string name() const = 0;
            };

        } // namespace algorithms
    } // namespace graph
} // namespace nexora


#endif //GITIGNORE_ALGORITHMBASE_H
