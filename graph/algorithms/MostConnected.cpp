//
// Created by HOME on 7/3/2026.
//
/**
 * @file graph/algorithms/MostConnected.cpp
 * @brief الگوریتم MostConnected — LockAlgorithm
 *
 * ═══════════════════════════════════════════════════════════════
 * هدف
 * ═══════════════════════════════════════════════════════════════
 * پیدا کردن تأثیرگذارترین nodes گراف بر اساس degree.
 * به سه معیار مختلف جواب می‌دهد:
 *
 *   out_degree  → چه کسی بیشترین followingها را دارد؟
 *   in_degree   → چه کسی بیشترین followerها را دارد؟ (influencers)
 *   total       → مجموع هر دو (محبوب‌ترین گره در شبکه)
 *
 * ═══════════════════════════════════════════════════════════════
 * روش و پیچیدگی
 * ═══════════════════════════════════════════════════════════════
 * روش: Degree Cache Lookup
 *
 * degree در NodeRecord از قبل cache شده است.
 * یعنی نیازی به iterate روی adj list نیست — فقط یک pass روی
 * همه nodes برای مقایسه کافی است.
 *
 *   پیچیدگی: O(V) برای sort و top-K
 *   مزیت: بدون شمارش دستی — هیچ forEachOutEdge لازم نیست
 *
 * ═══════════════════════════════════════════════════════════════
 * پارامترها
 * ═══════════════════════════════════════════════════════════════
 *   params[0]: limit     — تعداد نتایج (پیش‌فرض: 10)
 *   params[1]: metric    — "in" | "out" | "total" (پیش‌فرض: "in")
 *   params[2]: node_type — فیلتر نوع node (پیش‌فرض: همه)
 *
 * مثال:
 *   ["20", "in", "User"]  → ۲۰ کاربر با بیشترین follower
 *   ["5",  "out"]         → ۵ node با بیشترین following
 *   ["10", "total"]       → ۱۰ node با بیشترین degree کل
 *
 * ═══════════════════════════════════════════════════════════════
 * خروجی JSON
 * ═══════════════════════════════════════════════════════════════
 * {
 *   "metric":  "in",
 *   "limit":   10,
 *   "results": [
 *     {"id": "u_001", "type": "User", "in": 5420, "out": 300, "total": 5720},
 *     {"id": "u_002", "type": "User", "in": 3100, "out": 220, "total": 3320},
 *     ...
 *   ]
 * }
 */

#include "BuiltinAlgorithms.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <vector>

namespace nexora {
    namespace graph {
        namespace algorithms {

            class MostConnected : public LockAlgorithm {
            public:
                std::string name() const override { return "MostConnected"; }

                AlgoResult run(const LiveGraph&          graph,
                               const std::vector<ExtId>& params) override
                {
                    auto t0 = std::chrono::steady_clock::now();

                    // ── ۱. پارامترها ─────────────────────────────────────

                    // تعداد نتایج
                    size_t limit = 10;
                    if (params.size() >= 1 && !params[0].empty()) {
                        try { limit = static_cast<size_t>(std::stoul(params[0])); }
                        catch (...) { limit = 10; }
                    }
                    if (limit == 0 || limit > 1000) limit = 10;

                    // معیار مرتب‌سازی
                    // "in"    → follower count (چه کسی محبوب‌ترین است)
                    // "out"   → following count (چه کسی فعال‌ترین است)
                    // "total" → مجموع (بیشترین حضور کلی)
                    std::string metric = "in";
                    if (params.size() >= 2 && !params[1].empty()) {
                        const std::string& m = params[1];
                        if (m == "out" || m == "total") metric = m;
                    }

                    // فیلتر نوع node (اختیاری)
                    std::string filter_type;
                    if (params.size() >= 3) filter_type = params[2];

                    // TypeId فیلتر (اگر داده شده)
                    TypeId filter_tid = kInvalidTypeId;
                    if (!filter_type.empty()) {
                        auto tid = graph.getNodeTypeId(filter_type);
                        if (!tid) {
                            return AlgoResult{false,
                                              "Node type not found: " + filter_type,
                                              "",
                                              0.0};
                        }
                        filter_tid = *tid;
                    }

                    // ── ۲. جمع‌آوری nodes با degree ──────────────────────
                    // degree از NodeRecord.out_degree / in_degree خوانده می‌شود
                    // این مقادیر از قبل cache شده‌اند — هیچ iterate روی adj لازم نیست

                    struct NodeDegree {
                        DenseId  dense_id;
                        uint64_t out_degree;
                        uint64_t in_degree;
                        uint64_t total;
                        TypeId   type_id;
                    };

                    std::vector<NodeDegree> candidates;
                    candidates.reserve(graph.activeNodeCount());

                    graph.forEachNode([&](DenseId id, const NodeRecord& rec) -> bool {
                        // فیلتر نوع
                        if (filter_tid != kInvalidTypeId && rec.type_id != filter_tid)
                            return true;

                        candidates.push_back({
                                                     id,
                                                     rec.out_degree,
                                                     rec.in_degree,
                                                     rec.out_degree + rec.in_degree,
                                                     rec.type_id
                                             });
                        return true;
                    });

                    if (candidates.empty()) {
                        return AlgoResult{true, "",
                                          "{\"metric\":\"" + metric +
                                          "\",\"limit\":" + std::to_string(limit) +
                                          ",\"results\":[]}", 0.0};
                    }

                    // ── ۳. partial_sort — فقط top-K نیاز داریم ──────────
                    // O(V · log K) — بهتر از sort کامل O(V · log V)

                    const size_t k = std::min(limit, candidates.size());

                    auto comparator = [&metric](const NodeDegree& a, const NodeDegree& b) {
                        if      (metric == "out")   return a.out_degree > b.out_degree;
                        else if (metric == "total") return a.total      > b.total;
                        else                        return a.in_degree  > b.in_degree; // "in"
                    };

                    std::partial_sort(candidates.begin(),
                                      candidates.begin() + k,
                                      candidates.end(),
                                      comparator);

                    // ── ۴. ساخت JSON ─────────────────────────────────────

                    std::ostringstream json;
                    json << "{"
                         << "\"metric\":\"" << metric << "\""
                         << ",\"limit\":"   << limit
                         << ",\"total_scanned\":" << candidates.size()
                         << ",\"results\":[";

                    for (size_t i = 0; i < k; ++i) {
                        if (i) json << ",";
                        const auto& nd = candidates[i];
                        json << "{"
                             << "\"id\":\""    << graph.getExtId(nd.dense_id)  << "\""
                             << ",\"type\":\"" << graph.getNodeTypeName(nd.type_id) << "\""
                             << ",\"in\":"     << nd.in_degree
                             << ",\"out\":"    << nd.out_degree
                             << ",\"total\":"  << nd.total
                             << "}";
                    }
                    json << "]}";

                    double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count();

                    return AlgoResult{true, "", json.str(), ms};
                }
            };

            AlgoResult runMostConnected(GraphManager& manager,
                                        const std::string& graph_name,
                                        const std::vector<ExtId>& params) {
                MostConnected algo;
                return manager.runLock(graph_name, algo, params);
            }

        } // namespace algorithms
    } // namespace graph
} // namespace nexora