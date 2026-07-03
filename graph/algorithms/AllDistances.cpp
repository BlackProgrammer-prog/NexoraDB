//
// Created by HOME on 7/3/2026.
//
/**
 * @file graph/algorithms/AllDistances.cpp
 * @brief AllDistances — JobAlgorithm (BFS-SSSP)
 *
 * ═══════════════════════════════════════════════════════════════
 * هدف
 * ═══════════════════════════════════════════════════════════════
 * محاسبه کوتاه‌ترین فاصله (hop count) از یک node به بقیه.
 * در حالت پیشرفته (all_pairs): فاصله همه nodes از همه nodes.
 *
 * این الگوریتم دو کاربرد مستقل دارد:
 *
 *   حالت ۱ — SSSP (Single Source):
 *     از یک node مشخص به همه nodes دیگر.
 *     نتیجه: لیست مرتب از نزدیک‌ترین تا دورترین node.
 *
 *   حالت ۲ — All-Pairs:
 *     از هر node به هر node دیگر (N بار BFS).
 *     نتیجه: برای هر node یک لیست مرتب از کمترین تا بیشترین فاصله.
 *     هشدار: O(V × (V+E)) — فقط روی گراف‌های کوچک تا متوسط اجرا کنید.
 *
 * ═══════════════════════════════════════════════════════════════
 * روش — BFS (نه Dijkstra)
 * ═══════════════════════════════════════════════════════════════
 * گراف ما unweighted است.
 * در unweighted graph:
 *   BFS   → O(V+E) ← ما از این استفاده می‌کنیم ✅
 *   Dijkstra → O((V+E) log V) ← اضافه و کندتر ❌
 *
 * BFS level-by-level کوتاه‌ترین مسیر را به‌طور طبیعی پیدا می‌کند.
 *
 * ═══════════════════════════════════════════════════════════════
 * پارامترها
 * ═══════════════════════════════════════════════════════════════
 *   params[0]: src_id   — ExtId مبدا   (مثال: "u1")
 *   params[1]: "all"    — اگر "all" باشد → All-Pairs (اختیاری)
 *   params[2]: max_hops — حداکثر عمق BFS (پیش‌فرض: 6)
 *   params[3]: node_type — فیلتر node type در خروجی (پیش‌فرض: همه)
 *
 * مثال‌ها:
 *   ["u1"]               → SSSP از u1 به همه (تا بی‌نهایت hop)
 *   ["u1", "",   "3"]    → SSSP از u1 با max_hops=3 (دوستان تا سطح ۳)
 *   ["",  "all", "4"]    → All-Pairs با max_hops=4
 *   ["u1", "",   "6", "User"] → SSSP، فقط nodes از نوع User در خروجی
 *
 * ═══════════════════════════════════════════════════════════════
 * خروجی JSON
 * ═══════════════════════════════════════════════════════════════
 * حالت SSSP:
 * {
 *   "mode": "sssp",
 *   "source": "u1",
 *   "max_hops": 6,
 *   "reached": 47,
 *   "unreachable": 3,
 *   "distances": [
 *     {"id":"u2","type":"User","distance":1},
 *     {"id":"u3","type":"User","distance":1},
 *     {"id":"u5","type":"User","distance":2},
 *     ...
 *   ]
 * }
 *
 * حالت All-Pairs:
 * {
 *   "mode": "all_pairs",
 *   "max_hops": 4,
 *   "sources": [
 *     {
 *       "source": "u1",
 *       "distances": [
 *         {"id":"u2","distance":1},
 *         {"id":"u3","distance":2},
 *         ...
 *       ]
 *     },
 *     ...
 *   ]
 * }
 */

#include "BuiltinAlgorithms.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace nexora {
    namespace graph {
        namespace algorithms {

            class AllDistances : public JobAlgorithm {
            public:
                std::string name() const override { return "AllDistances"; }

                AlgoResult run(const StaticGraph&        snapshot,
                               const std::vector<ExtId>& params) override
                {
                    auto t0 = std::chrono::steady_clock::now();

                    const uint64_t N = snapshot.nodeCount();
                    if (N == 0)
                        return AlgoResult{true, "", "{\"mode\":\"sssp\",\"reached\":0}", 0.0};

                    // ── ۱. خواندن پارامترها ──────────────────────────────

                    ExtId src_ext;
                    bool  all_pairs  = false;
                    int   max_hops   = std::numeric_limits<int>::max(); // بدون محدودیت
                    std::string filter_type;

                    if (params.size() >= 1) src_ext = params[0];

                    if (params.size() >= 2 && params[1] == "all") {
                        all_pairs = true;
                        src_ext   = "";
                    }

                    if (params.size() >= 3 && !params[2].empty()) {
                        try { max_hops = std::stoi(params[2]); }
                        catch (...) { max_hops = 6; }
                    }
                    if (max_hops <= 0) max_hops = 6;

                    if (params.size() >= 4) filter_type = params[3];

                    // ── ۲. فیلتر node type (اختیاری) ────────────────────

                    TypeId filter_tid = kInvalidTypeId;
                    if (!filter_type.empty()) {
                        // برای StaticGraph از nodeType() + nodeTypeName() استفاده می‌کنیم
                        snapshot.forEachNode([&](DenseId id, TypeId tid) -> bool {
                            if (snapshot.nodeTypeName(tid) == filter_type) {
                                filter_tid = tid;
                                return false; // یافتیم — توقف
                            }
                            return true;
                        });
                    }

                    // ── ۳. تابع BFS از یک مبدا ──────────────────────────

                    // خروجی هر BFS: DenseId → فاصله
                    // فقط nodes قابل دسترس در max_hops
                    struct DistEntry {
                        DenseId dense_id;
                        int     distance;
                        TypeId  type_id;
                    };

                    auto bfs = [&](DenseId src) -> std::vector<DistEntry> {
                        // dist: -1 = ندیده
                        std::unordered_map<DenseId, int> dist;
                        dist.reserve(N);
                        dist[src] = 0;

                        std::queue<DenseId> q;
                        q.push(src);

                        while (!q.empty()) {
                            DenseId cur = q.front(); q.pop();
                            int     d   = dist[cur];

                            if (d >= max_hops) continue; // از این عمق بیشتر نرو

                            // iterate روی neighbors خروجی
                            snapshot.forEachNeighbor(cur, Direction::Out,
                                                     [&](DenseId nbr, TypeId) -> bool {
                                                         if (dist.find(nbr) == dist.end()) {
                                                             dist[nbr] = d + 1;
                                                             q.push(nbr);
                                                         }
                                                         return true;
                                                     });

                            // اگر گراف undirected است، IN را هم بگیر
                            // (برای directed graph فقط Out کافی است)
                        }

                        // تبدیل dist map به vector + مرتب‌سازی
                        std::vector<DistEntry> result;
                        result.reserve(dist.size());
                        for (const auto& [id, d] : dist) {
                            if (id == src) continue; // مبدا خودش را شامل نمی‌شود
                            // فیلتر type
                            TypeId tid = snapshot.nodeType(id);
                            if (filter_tid != kInvalidTypeId && tid != filter_tid) continue;
                            result.push_back({id, d, tid});
                        }

                        // مرتب‌سازی: کمترین فاصله اول، در فاصله مساوی ExtId
                        std::sort(result.begin(), result.end(),
                                  [&](const DistEntry& a, const DistEntry& b) {
                                      if (a.distance != b.distance) return a.distance < b.distance;
                                      return snapshot.extId(a.dense_id) < snapshot.extId(b.dense_id);
                                  });

                        return result;
                    };

                    // ── ۴. helper برای ساخت JSON یک DistEntry ───────────

                    auto entryToJson = [&](const DistEntry& e, bool include_type) -> std::string {
                        std::ostringstream s;
                        s << "{\"id\":\"" << snapshot.extId(e.dense_id) << "\""
                          << ",\"distance\":" << e.distance;
                        if (include_type)
                            s << ",\"type\":\"" << snapshot.nodeTypeName(e.type_id) << "\"";
                        s << "}";
                        return s.str();
                    };

                    // ═══════════════════════════════════════════════════════
                    // حالت ۱: SSSP — از یک مبدا
                    // ═══════════════════════════════════════════════════════
                    if (!all_pairs) {
                        if (src_ext.empty())
                            return AlgoResult{false, "params[0]: source node id required", "", 0.0};

                        DenseId src = snapshot.denseId(src_ext);
                        if (src == kInvalidDenseId)
                            return AlgoResult{false, "Source node not found: " + src_ext, "", 0.0};

                        auto distances = bfs(src);

                        // آمار
                        int   max_dist   = distances.empty() ? 0 : distances.back().distance;
                        size_t unreachable = N - distances.size() - 1; // -1 برای خود src

                        std::ostringstream json;
                        json << "{"
                             << "\"mode\":\"sssp\""
                             << ",\"source\":\"" << src_ext << "\""
                             << ",\"max_hops\":"  << (max_hops == std::numeric_limits<int>::max()
                                                      ? -1 : max_hops)
                             << ",\"reached\":"   << distances.size()
                             << ",\"unreachable\":" << unreachable
                             << ",\"max_distance_found\":" << max_dist
                             << ",\"distances\":[";

                        for (size_t i = 0; i < distances.size(); ++i) {
                            if (i) json << ",";
                            json << entryToJson(distances[i], true);
                        }
                        json << "]}";

                        double ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t0).count();

                        return AlgoResult{true, "", json.str(), ms};
                    }

                    // ═══════════════════════════════════════════════════════
                    // حالت ۲: All-Pairs — از همه nodes
                    // ═══════════════════════════════════════════════════════
                    // هشدار: O(V × (V+E)) — فقط برای گراف‌های کوچک/متوسط

                    // جمع‌آوری همه DenseId ها
                    std::vector<DenseId> all_nodes;
                    all_nodes.reserve(N);
                    snapshot.forEachNode([&](DenseId id, TypeId) -> bool {
                        all_nodes.push_back(id);
                        return true;
                    });

                    std::ostringstream json;
                    json << "{"
                         << "\"mode\":\"all_pairs\""
                         << ",\"max_hops\":"       << max_hops
                         << ",\"node_count\":"     << all_nodes.size()
                         << ",\"sources\":[";

                    for (size_t si = 0; si < all_nodes.size(); ++si) {
                        if (si) json << ",";

                        DenseId     src     = all_nodes[si];
                        ExtId       src_ext_i = snapshot.extId(src);
                        auto        dists   = bfs(src);

                        json << "{"
                             << "\"source\":\"" << src_ext_i << "\""
                             << ",\"reached\":"  << dists.size()
                             << ",\"distances\":[";

                        // برای all-pairs، type را حذف می‌کنیم تا JSON کوچک‌تر باشد
                        for (size_t di = 0; di < dists.size(); ++di) {
                            if (di) json << ",";
                            json << entryToJson(dists[di], false);
                        }
                        json << "]}";
                    }
                    json << "]}";

                    double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count();

                    return AlgoResult{true, "", json.str(), ms};
                }
            };

            AlgoResult runAllDistances(GraphManager& manager,
                                       const std::string& graph_name,
                                       const std::vector<ExtId>& params) {
                AllDistances algo;
                auto handle = manager.submitJob(graph_name, algo, params);
                return handle.result();
            }

        } // namespace algorithms
    } // namespace graph
} // namespace nexora