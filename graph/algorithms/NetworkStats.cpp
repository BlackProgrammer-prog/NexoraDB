//
// Created by HOME on 7/3/2026.
//
/**
 * @file graph/algorithms/NetworkStats.cpp
 * @brief الگوریتم NetworkStats — LockAlgorithm
 *
 * ═══════════════════════════════════════════════════════════════
 * هدف
 * ═══════════════════════════════════════════════════════════════
 * یک snapshot کامل از وضعیت آماری گراف زنده.
 * برای داشبورد ادمین، health monitoring، و debugging.
 *
 * این الگوریتم اطلاعات را از چند منبع جمع می‌کند:
 *
 *   ① GraphStats  — شمارنده‌های atomic O(1):
 *     active_nodes, active_edges, deleted_nodes, heavy_nodes, version
 *
 *   ② یک pass روی nodes — O(V):
 *     توزیع degree (min/max/avg/median)
 *     تعداد nodes بر اساس type
 *     تعداد heavy nodes (nodes با degree > 1024)
 *     isolated nodes (degree = 0)
 *
 *   ③ یک pass روی edges — O(E):
 *     تعداد edges بر اساس type
 *     density گراف
 *
 * ═══════════════════════════════════════════════════════════════
 * روش و پیچیدگی
 * ═══════════════════════════════════════════════════════════════
 * روش: Incremental Counters + Single Pass
 *
 *   آمارهای اصلی: O(1)  ← از GraphStats cache
 *   توزیع degree:  O(V)  ← یک pass روی nodes
 *   توزیع edge type: O(E) ← یک pass روی edges
 *
 * ═══════════════════════════════════════════════════════════════
 * پارامترها
 * ═══════════════════════════════════════════════════════════════
 *   params[0]: "full" | "basic" | "degree" | "types"
 *              basic  = فقط شمارنده‌های O(1) (سریع‌ترین)
 *              degree = basic + توزیع degree
 *              types  = basic + تعداد بر اساس type
 *              full   = همه موارد (پیش‌فرض)
 *
 * ═══════════════════════════════════════════════════════════════
 * خروجی JSON
 * ═══════════════════════════════════════════════════════════════
 * {
 *   "mode": "full",
 *   "basic": {
 *     "active_nodes": 50000,
 *     "active_edges": 1200000,
 *     "deleted_nodes": 230,
 *     "deleted_edges": 450,
 *     "heavy_nodes": 12,
 *     "graph_version": 8742
 *   },
 *   "degree": {
 *     "out": {"min":0, "max":5420, "avg":24.0, "median":8},
 *     "in":  {"min":0, "max":9800, "avg":24.0, "median":3},
 *     "isolated_nodes": 142
 *   },
 *   "node_types": {
 *     "User": 45000,
 *     "Post": 5000
 *   },
 *   "edge_types": {
 *     "FOLLOWS": 1000000,
 *     "LIKES":   200000
 *   },
 *   "density": 0.00048
 * }
 */

#include "BuiltinAlgorithms.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace nexora {
    namespace graph {
        namespace algorithms {

            class NetworkStats : public LockAlgorithm {
            public:
                std::string name() const override { return "NetworkStats"; }

                AlgoResult run(const LiveGraph&          graph,
                               const std::vector<ExtId>& params) override
                {
                    auto t0 = std::chrono::steady_clock::now();

                    // ── ۱. mode انتخاب ───────────────────────────────────
                    // basic  = فقط شمارنده‌های O(1)
                    // degree = basic + توزیع degree
                    // types  = basic + توزیع type
                    // full   = همه (پیش‌فرض)
                    std::string mode = "full";
                    if (!params.empty() && !params[0].empty()) {
                        const std::string& m = params[0];
                        if (m == "basic" || m == "degree" || m == "types")
                            mode = m;
                    }

                    const bool do_degree = (mode == "degree" || mode == "full");
                    const bool do_types  = (mode == "types"  || mode == "full");

                    // ── ۲. آمار پایه از GraphStats — O(1) ───────────────
                    // GraphStats از شمارنده‌های atomic استفاده می‌کند
                    // هیچ iterate ای لازم نیست
                    const GraphStats stats = graph.stats();

                    const uint64_t N = stats.active_nodes;
                    const uint64_t E = stats.active_edges;

                    // ── ۳. توزیع degree — O(V) ──────────────────────────
                    // اگر mode نیاز دارد
                    struct DegreeStats {
                        uint64_t min_val    = UINT64_MAX;
                        uint64_t max_val    = 0;
                        double   avg        = 0.0;
                        uint64_t median     = 0;
                        uint64_t isolated   = 0;  // nodes با degree = 0
                    };
                    DegreeStats out_deg_stats, in_deg_stats;

                    // توزیع type
                    std::unordered_map<std::string, uint64_t> node_type_count;
                    std::unordered_map<std::string, uint64_t> edge_type_count;

                    if (do_degree || do_types) {
                        // یک pass روی nodes — هر دو degree و type را جمع می‌کنیم
                        std::vector<uint64_t> out_degrees, in_degrees;
                        if (do_degree) {
                            out_degrees.reserve(N);
                            in_degrees.reserve(N);
                        }

                        graph.forEachNode([&](DenseId id, const NodeRecord& rec) -> bool {
                            // degree از NodeRecord.out_degree / in_degree — O(1)
                            if (do_degree) {
                                out_degrees.push_back(rec.out_degree);
                                in_degrees.push_back(rec.in_degree);

                                // isolated: هر دو out و in صفر
                                if (rec.out_degree == 0 && rec.in_degree == 0)
                                    ++out_deg_stats.isolated;
                            }

                            // type distribution
                            if (do_types) {
                                std::string tname = graph.getNodeTypeName(rec.type_id);
                                if (tname.empty()) tname = "unknown";
                                node_type_count[tname]++;
                            }

                            return true;
                        });

                        // محاسبه آمار توزیع degree
                        if (do_degree && !out_degrees.empty()) {
                            // out_degree
                            auto [mn_o, mx_o] = std::minmax_element(
                                    out_degrees.begin(), out_degrees.end());
                            out_deg_stats.min_val = *mn_o;
                            out_deg_stats.max_val = *mx_o;
                            out_deg_stats.avg     = static_cast<double>(
                                                            std::accumulate(out_degrees.begin(), out_degrees.end(), 0ULL))
                                                    / out_degrees.size();

                            // median — partial_sort برای بزرگ‌ترین V/2 کافی است
                            size_t mid = out_degrees.size() / 2;
                            std::nth_element(out_degrees.begin(),
                                             out_degrees.begin() + mid,
                                             out_degrees.end());
                            out_deg_stats.median = out_degrees[mid];

                            // in_degree
                            auto [mn_i, mx_i] = std::minmax_element(
                                    in_degrees.begin(), in_degrees.end());
                            in_deg_stats.min_val = *mn_i;
                            in_deg_stats.max_val = *mx_i;
                            in_deg_stats.avg     = static_cast<double>(
                                                           std::accumulate(in_degrees.begin(), in_degrees.end(), 0ULL))
                                                   / in_degrees.size();
                            in_deg_stats.isolated = out_deg_stats.isolated; // همان nodes

                            size_t mid_i = in_degrees.size() / 2;
                            std::nth_element(in_degrees.begin(),
                                             in_degrees.begin() + mid_i,
                                             in_degrees.end());
                            in_deg_stats.median = in_degrees[mid_i];
                        }
                    }

                    if (do_types) {
                        // یک pass روی edges برای توزیع edge type
                        graph.forEachEdge([&](EdgeId, const EdgeRecord& er) -> bool {
                            std::string tname = graph.getEdgeTypeName(er.type_id);
                            if (tname.empty()) tname = "unknown";
                            edge_type_count[tname]++;
                            return true;
                        });
                    }

                    // ── ۴. density گراف ──────────────────────────────────
                    // density = E / (V × (V-1))  برای directed graph
                    // density = 0 اگر کمتر از ۲ node داریم
                    double density = 0.0;
                    if (N >= 2) {
                        density = static_cast<double>(E)
                                  / (static_cast<double>(N) * (N - 1));
                    }

                    // ── ۵. ساخت JSON ─────────────────────────────────────
                    std::ostringstream json;

                    // helper: اگر double کوچک است، با precision ۶ رقم نشان بده
                    auto fmtDouble = [](double v, int prec = 2) -> std::string {
                        std::ostringstream s;
                        s << std::fixed;
                        s.precision(prec);
                        s << v;
                        return s.str();
                    };

                    json << "{"
                         << "\"mode\":\"" << mode << "\""

                         // ── Basic stats (همیشه) ──
                         << ",\"basic\":{"
                         << "\"active_nodes\":"  << N
                         << ",\"active_edges\":" << E
                         << ",\"deleted_nodes\":" << stats.deleted_nodes
                         << ",\"deleted_edges\":" << stats.deleted_edges
                         << ",\"heavy_nodes\":"  << stats.heavy_nodes
                         << ",\"graph_version\":" << stats.version
                         << "}";

                    // ── Degree distribution ──
                    if (do_degree) {
                        json << ",\"degree\":{"
                             << "\"out\":{"
                             << "\"min\":"    << (N > 0 ? out_deg_stats.min_val : 0)
                             << ",\"max\":"   << out_deg_stats.max_val
                             << ",\"avg\":"   << fmtDouble(out_deg_stats.avg)
                             << ",\"median\":" << out_deg_stats.median
                             << "},"
                             << "\"in\":{"
                             << "\"min\":"    << (N > 0 ? in_deg_stats.min_val : 0)
                             << ",\"max\":"   << in_deg_stats.max_val
                             << ",\"avg\":"   << fmtDouble(in_deg_stats.avg)
                             << ",\"median\":" << in_deg_stats.median
                             << "},"
                             << "\"isolated_nodes\":" << out_deg_stats.isolated
                             << "}";
                    }

                    // ── Node types ──
                    if (do_types) {
                        json << ",\"node_types\":{";
                        bool first = true;
                        for (const auto& [type, cnt] : node_type_count) {
                            if (!first) json << ",";
                            json << "\"" << type << "\":" << cnt;
                            first = false;
                        }
                        json << "}";

                        json << ",\"edge_types\":{";
                        first = true;
                        for (const auto& [type, cnt] : edge_type_count) {
                            if (!first) json << ",";
                            json << "\"" << type << "\":" << cnt;
                            first = false;
                        }
                        json << "}";
                    }

                    // ── Density ──
                    json << ",\"density\":"     << fmtDouble(density, 6)
                         << "}";

                    double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count();

                    return AlgoResult{true, "", json.str(), ms};
                }
            };

            AlgoResult runNetworkStats(GraphManager& manager,
                                       const std::string& graph_name,
                                       const std::vector<ExtId>& params) {
                NetworkStats algo;
                return manager.runLock(graph_name, algo, params);
            }

        } // namespace algorithms
    } // namespace graph
} // namespace nexora