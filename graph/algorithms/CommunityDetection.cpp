//
// Created by HOME on 7/3/2026.
//
/**
 * @file graph/algorithms/CommunityDetection.cpp
 * @brief CommunityDetection — JobAlgorithm (Label Propagation)
 *
 * ═══════════════════════════════════════════════════════════════
 * هدف
 * ═══════════════════════════════════════════════════════════════
 * کشف گروه‌های طبیعی (Community) در شبکه اجتماعی.
 * نوشته شده برای شبکه‌های بزرگ (میلیون‌ها node).
 *
 * خروجی دو قابلیت مجزا دارد:
 *
 *   قابلیت ۱ — Community Summary:
 *     تعداد کل communities، بزرگ‌ترین‌ها، و آمار کلی.
 *
 *   قابلیت ۲ — Member Listing (جدید):
 *     لیست کامل اعضای هر community.
 *     هر گروه = {id, اعضا[], سایز}.
 *     مرتب از بزرگ‌ترین به کوچک‌ترین.
 *
 * ═══════════════════════════════════════════════════════════════
 * روش — Label Propagation Algorithm (LPA)
 * ═══════════════════════════════════════════════════════════════
 * چرا LPA و نه Louvain؟
 *   Louvain: دقیق‌تر اما پیچیده‌تر و کندتر (O(V log V) per pass)
 *   LPA:     سریع‌تر (O(V+E) per pass)، نتایج خوب برای شبکه‌های بزرگ
 *
 * جریان LPA:
 *   ۱. هر node یک label منحصر (DenseId خودش) دارد
 *   ۲. در هر iteration:
 *      هر node → label اکثریت همسایگانش را می‌گیرد
 *   ۳. تکرار تا همگرایی (یا max_iter)
 *   ۴. nodes با label یکسان = یک community
 *
 * پیچیدگی: O(iter × (V+E)) — معمولاً ۵-۱۰ iteration کافی است
 *
 * ═══════════════════════════════════════════════════════════════
 * پارامترها
 * ═══════════════════════════════════════════════════════════════
 *   params[0]: max_iterations     — حداکثر تعداد iteration (پیش‌فرض: 10)
 *   params[1]: min_community_size — حداقل اعضای یک community (پیش‌فرض: 2)
 *   params[2]: "members"          — اگر "members" باشد، لیست اعضا هم می‌دهد
 *   params[3]: node_type          — فیلتر نوع node (پیش‌فرض: همه)
 *
 * مثال‌ها:
 *   []                          → نتایج پایه، min_size=2
 *   ["20", "5"]                 → ۲۰ iteration، communities با حداقل ۵ عضو
 *   ["10", "2", "members"]      → با لیست اعضا
 *   ["10", "3", "members","User"]→ فقط روی User nodes
 *
 * ═══════════════════════════════════════════════════════════════
 * خروجی JSON
 * ═══════════════════════════════════════════════════════════════
 * {
 *   "algorithm": "label_propagation",
 *   "iterations_run": 7,
 *   "total_communities": 12,
 *   "total_nodes_assigned": 498,
 *   "isolated_nodes": 2,
 *   "summary": {
 *     "largest_community_size": 120,
 *     "smallest_community_size": 2,
 *     "avg_community_size": 41.5
 *   },
 *   "communities": [
 *     {
 *       "community_id": "c_0",
 *       "size": 120,
 *       "members": ["u1","u5","u9","u12", ...]  ← اگر params[2]="members"
 *     },
 *     {
 *       "community_id": "c_1",
 *       "size": 95,
 *       "members": ["u2","u6","u14", ...]
 *     },
 *     ...
 *   ]
 * }
 */

#include "BuiltinAlgorithms.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace nexora {
    namespace graph {
        namespace algorithms {

            class CommunityDetection : public JobAlgorithm {
            public:
                std::string name() const override { return "CommunityDetection"; }

                AlgoResult run(const StaticGraph&        snapshot,
                               const std::vector<ExtId>& params) override
                {
                    auto t0 = std::chrono::steady_clock::now();

                    const uint64_t N = snapshot.nodeCount();
                    if (N == 0)
                        return AlgoResult{true, "",
                                          "{\"algorithm\":\"label_propagation\","
                                          "\"total_communities\":0,\"communities\":[]}", 0.0};

                    // ── ۱. پارامترها ─────────────────────────────────────

                    int    max_iter   = 10;
                    size_t min_size   = 2;
                    bool   with_members = false;
                    std::string filter_type;

                    if (params.size() >= 1 && !params[0].empty()) {
                        try { max_iter = std::stoi(params[0]); } catch (...) {}
                    }
                    if (max_iter <= 0 || max_iter > 100) max_iter = 10;

                    if (params.size() >= 2 && !params[1].empty()) {
                        try { min_size = static_cast<size_t>(std::stoul(params[1])); }
                        catch (...) {}
                    }
                    if (min_size == 0) min_size = 2;

                    if (params.size() >= 3 && params[2] == "members")
                        with_members = true;

                    if (params.size() >= 4) filter_type = params[3];

                    // ── ۲. جمع‌آوری nodes با فیلتر ───────────────────────

                    TypeId filter_tid = kInvalidTypeId;
                    if (!filter_type.empty()) {
                        snapshot.forEachNode([&](DenseId id, TypeId tid) -> bool {
                            if (snapshot.nodeTypeName(tid) == filter_type) {
                                filter_tid = tid;
                                return false;
                            }
                            return true;
                        });
                    }

                    // index پیوسته برای nodes — LPA نیاز به array دارد
                    std::vector<DenseId> nodes;
                    nodes.reserve(N);
                    std::unordered_map<DenseId, uint32_t> node_to_idx;
                    node_to_idx.reserve(N);

                    snapshot.forEachNode([&](DenseId id, TypeId tid) -> bool {
                        if (filter_tid != kInvalidTypeId && tid != filter_tid)
                            return true;
                        node_to_idx[id] = static_cast<uint32_t>(nodes.size());
                        nodes.push_back(id);
                        return true;
                    });

                    const uint32_t M = static_cast<uint32_t>(nodes.size());
                    if (M == 0)
                        return AlgoResult{false, "No nodes after filter", "", 0.0};

                    // ── ۳. مرحله ۱ LPA: initialization ──────────────────
                    // هر node label = index خودش (یعنی community مستقل)
                    std::vector<uint32_t> label(M);
                    for (uint32_t i = 0; i < M; ++i) label[i] = i;

                    // ── ۴. مرحله ۲ LPA: iteration ────────────────────────
                    // در هر iteration، هر node label اکثریت همسایگانش را می‌گیرد.
                    // اگر چند label تساوی داشتند، کوچک‌ترین را انتخاب می‌کنیم (deterministic).

                    // ترتیب تصادفی برای جلوگیری از bias
                    std::vector<uint32_t> order(M);
                    for (uint32_t i = 0; i < M; ++i) order[i] = i;

                    // seed ثابت برای reproducibility
                    std::mt19937 rng(42);

                    int actual_iter = 0;
                    for (int iter = 0; iter < max_iter; ++iter) {
                        ++actual_iter;
                        bool changed = false;

                        std::shuffle(order.begin(), order.end(), rng);

                        for (uint32_t oi = 0; oi < M; ++oi) {
                            uint32_t idx = order[oi];
                            DenseId  cur = nodes[idx];

                            // شمارش label همسایگان
                            std::unordered_map<uint32_t, int> label_count;

                            // OUT neighbors
                            snapshot.forEachNeighbor(cur, Direction::Both,
                                                     [&](DenseId nbr, TypeId) -> bool {
                                                         auto it = node_to_idx.find(nbr);
                                                         if (it != node_to_idx.end())
                                                             label_count[label[it->second]]++;
                                                         return true;
                                                     });

                            if (label_count.empty()) continue; // isolated node

                            // label با بیشترین تعداد (در تساوی، کوچک‌ترین)
                            uint32_t best_label = label[idx];
                            int      best_count = 0;

                            for (const auto& [lbl, cnt] : label_count) {
                                if (cnt > best_count ||
                                    (cnt == best_count && lbl < best_label)) {
                                    best_count = cnt;
                                    best_label = lbl;
                                }
                            }

                            if (best_label != label[idx]) {
                                label[idx] = best_label;
                                changed = true;
                            }
                        }

                        // همگرایی
                        if (!changed) break;
                    }

                    // ── ۵. گروه‌بندی nodes بر اساس label ─────────────────

                    // label → لیست index های node
                    std::unordered_map<uint32_t, std::vector<uint32_t>> communities;
                    communities.reserve(M / 4); // تخمین اولیه

                    for (uint32_t i = 0; i < M; ++i)
                        communities[label[i]].push_back(i);

                    // ── ۶. فیلتر بر اساس min_size ────────────────────────

                    // جداسازی isolated nodes (community با ۱ عضو)
                    size_t isolated_count = 0;

                    std::vector<std::vector<uint32_t>> valid_communities;
                    valid_communities.reserve(communities.size());

                    for (auto& [lbl, members] : communities) {
                        if (members.size() < min_size)
                            isolated_count += members.size();
                        else
                            valid_communities.push_back(std::move(members));
                    }

                    // مرتب‌سازی: بزرگ‌ترین اول
                    std::sort(valid_communities.begin(), valid_communities.end(),
                              [](const auto& a, const auto& b) {
                                  return a.size() > b.size();
                              });

                    // ── ۷. آمار ──────────────────────────────────────────

                    size_t total_assigned = 0;
                    size_t max_size       = 0;
                    size_t min_comm_size  = SIZE_MAX;

                    for (const auto& c : valid_communities) {
                        total_assigned += c.size();
                        if (c.size() > max_size)      max_size      = c.size();
                        if (c.size() < min_comm_size) min_comm_size = c.size();
                    }

                    double avg_size = valid_communities.empty()
                                      ? 0.0
                                      : static_cast<double>(total_assigned)
                                        / valid_communities.size();

                    // ── ۸. ساخت JSON ─────────────────────────────────────

                    std::ostringstream json;
                    json << "{"
                         << "\"algorithm\":\"label_propagation\""
                         << ",\"iterations_run\":"       << actual_iter
                         << ",\"total_communities\":"    << valid_communities.size()
                         << ",\"total_nodes_assigned\":" << total_assigned
                         << ",\"isolated_nodes\":"       << isolated_count
                         << ",\"summary\":{"
                         << "\"largest_community_size\":"  << (max_size == 0 ? 0 : max_size)
                         << ",\"smallest_community_size\":" << (min_comm_size == SIZE_MAX ? 0 : min_comm_size)
                         << ",\"avg_community_size\":"
                         << [&]() -> std::string {
                             std::ostringstream s;
                             s << std::fixed; s.precision(1); s << avg_size;
                             return s.str();
                         }()
                         << "}"
                         << ",\"communities\":[";

                    for (size_t ci = 0; ci < valid_communities.size(); ++ci) {
                        if (ci) json << ",";
                        const auto& members = valid_communities[ci];

                        json << "{"
                             << "\"community_id\":\"c_" << ci << "\""
                             << ",\"size\":"             << members.size();

                        // ── قابلیت اضافه: لیست اعضا ──
                        if (with_members) {
                            json << ",\"members\":[";
                            // اعضا را بر اساس ExtId مرتب می‌کنیم (deterministic خروجی)
                            std::vector<ExtId> ext_members;
                            ext_members.reserve(members.size());
                            for (uint32_t idx : members)
                                ext_members.push_back(snapshot.extId(nodes[idx]));
                            std::sort(ext_members.begin(), ext_members.end());

                            for (size_t mi = 0; mi < ext_members.size(); ++mi) {
                                if (mi) json << ",";
                                json << "\"" << ext_members[mi] << "\"";
                            }
                            json << "]";
                        }

                        json << "}";
                    }

                    json << "]}";

                    double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count();

                    return AlgoResult{true, "", json.str(), ms};
                }
            };

            AlgoResult runCommunityDetection(GraphManager& manager,
                                             const std::string& graph_name,
                                             const std::vector<ExtId>& params) {
                CommunityDetection algo;
                auto handle = manager.submitJob(graph_name, algo, params);
                return handle.result();
            }

        } // namespace algorithms
    } // namespace graph
} // namespace nexora