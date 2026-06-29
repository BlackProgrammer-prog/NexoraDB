/**
 * @file graph/algorithms/ConnectedComponents.cpp
 * @brief ConnectedComponents — JobAlgorithm (نوع سنگین، background)
 *
 * ═══════════════════════════════════════════════════════════════
 * راهنما: JobAlgorithm چیست و چطور کار می‌کند؟
 * ═══════════════════════════════════════════════════════════════
 *
 * ۱. GraphManager یک snapshot از LiveGraph می‌گیرد (StaticGraph)
 * ۲. snapshot به background thread داده می‌شود
 * ۳. run() روی snapshot اجرا می‌شود — LiveGraph آزاد است، تغییر می‌کند
 * ۴. بعد از پایان، snapshot آزاد می‌شود، نتیجه برمی‌گردد
 *
 * در طول اجرا:
 *   ✅ snapshot.forEachNode()  — مجاز
 *   ✅ snapshot.forEachEdge()  — مجاز
 *   ✅ snapshot.neighbors()    — مجاز
 *   ✅ snapshot.extId()        — مجاز
 *   ✅ snapshot.nodeCount()    — مجاز
 *   ❌ LiveGraph مستقیم       — ممنوع (فقط snapshot)
 *   ❌ DocEngine               — ممنوع
 *   ❌ هر write operation      — ممنوع
 *
 * کِی از JobAlgorithm استفاده کن؟
 *   • روی کل گراف کار می‌کند (نه یک node)
 *   • بیشتر از ۲۰۰ms طول می‌کشد
 *   • کاربر می‌تواند منتظر نماند (async)
 * ═══════════════════════════════════════════════════════════════
 */

#include "BuiltinAlgorithms.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace nexora {
namespace graph {
namespace algorithms {

/**
 * @class ConnectedComponents
 * @brief پیدا کردن مؤلفه‌های متصل — Union-Find (DSU)
 *
 * @section چطور داده می‌خواند
 *
 * StaticGraph API که تیم الگوریتم استفاده می‌کند:
 *
 *   snapshot.nodeCount()
 *     → تعداد کل nodes در snapshot
 *
 *   snapshot.forEachNode(fn)
 *     → fn(DenseId, TypeId) → bool
 *     → iterate روی همه nodes فعال
 *
 *   snapshot.forEachEdge(fn)
 *     → fn(EdgeId, DenseId src, DenseId dst, TypeId) → bool
 *     → iterate روی همه edges — این کافی است برای DSU
 *
 *   snapshot.extId(DenseId)
 *     → برگشت به "_id" string برای خروجی JSON
 *
 *   snapshot.nodeTypeName(TypeId)
 *     → نام نوع node ("User", "Post", ...)
 *
 * @section چرا DSU و نه BFS؟
 *   DSU: یک pass روی edges → O(E·α(V)) ≈ O(E)
 *   BFS: برای هر component یک BFS جداگانه → پیچیده‌تر
 */
class ConnectedComponents : public JobAlgorithm {
public:
    std::string name() const override { return "ConnectedComponents"; }

    /**
     * @param snapshot  StaticGraph — read-only، thread-safe
     * @param params    [] — این الگوریتم پارامتر نمی‌خواهد
     *                  ["User"] — اختیاری: فقط روی نوع خاص
     */
    AlgoResult run(const StaticGraph& snapshot,
                   const std::vector<ExtId>& params) override
    {
        auto t0 = std::chrono::steady_clock::now();

        const uint64_t N = snapshot.nodeCount();
        if (N == 0) {
            return AlgoResult{true, "", "{\"components\":[]}", 0.0};
        }

        // ── ۱. ساخت DSU (Disjoint Set Union) ─────────────────
        // parent[i] = ریشه component که node i در آن است
        // rank[i]   = عمق درخت (برای union by rank)
        //
        // ⚠️ DenseId ها پیوسته نیستند لزوماً!
        // باید ابتدا همه DenseId های موجود را جمع‌آوری کنیم

        // جمع‌آوری DenseId های موجود در snapshot
        std::vector<DenseId> all_nodes;
        all_nodes.reserve(N);
        snapshot.forEachNode([&](DenseId id, TypeId tid) -> bool {
            // فیلتر نوع (اگر params[0] داده شده)
            if (!params.empty() && !params[0].empty()) {
                if (snapshot.nodeTypeName(tid) != params[0]) return true;
            }
            all_nodes.push_back(id);
            return true;
        });

        if (all_nodes.empty()) {
            return AlgoResult{true, "", "{\"components\":[]}", 0.0};
        }

        // DSU با map از DenseId به index
        // (چون DenseId ها ممکن است sparse باشند)
        std::unordered_map<DenseId, uint32_t> id_to_idx;
        id_to_idx.reserve(all_nodes.size());
        for (uint32_t i = 0; i < all_nodes.size(); ++i)
            id_to_idx[all_nodes[i]] = i;

        const uint32_t M = static_cast<uint32_t>(all_nodes.size());
        std::vector<uint32_t> parent(M);
        std::vector<uint32_t> rank(M, 0);
        std::iota(parent.begin(), parent.end(), 0);  // parent[i] = i

        // ── ۲. توابع DSU ──────────────────────────────────────

        // Find با Path Compression
        std::function<uint32_t(uint32_t)> find = [&](uint32_t x) -> uint32_t {
            if (parent[x] != x)
                parent[x] = find(parent[x]);  // path compression
            return parent[x];
        };

        // Union by Rank
        auto unite = [&](uint32_t a, uint32_t b) {
            uint32_t ra = find(a), rb = find(b);
            if (ra == rb) return;
            if (rank[ra] < rank[rb]) std::swap(ra, rb);
            parent[rb] = ra;
            if (rank[ra] == rank[rb]) ++rank[ra];
        };

        // ── ۳. یک pass روی edges → Union ─────────────────────
        // اینجا از snapshot.forEachEdge استفاده می‌کنیم
        // نه neighbors() — چون می‌خواهیم همه edges را یکجا ببینیم

        snapshot.forEachEdge([&](EdgeId, DenseId src, DenseId dst, TypeId) -> bool {
            auto it_src = id_to_idx.find(src);
            auto it_dst = id_to_idx.find(dst);
            if (it_src == id_to_idx.end() || it_dst == id_to_idx.end())
                return true;  // یکی از دو node در فیلتر نبود
            unite(it_src->second, it_dst->second);
            return true;  // ادامه بده
        });

        // ── ۴. گروه‌بندی nodes بر اساس component ─────────────

        // component_id → لیست DenseId های آن component
        std::unordered_map<uint32_t, std::vector<DenseId>> components;
        for (uint32_t i = 0; i < M; ++i) {
            uint32_t root = find(i);
            components[root].push_back(all_nodes[i]);
        }

        // مرتب‌سازی components از بزرگ به کوچک
        std::vector<std::pair<uint32_t, std::vector<DenseId>>> sorted_comps(
            components.begin(), components.end());
        std::sort(sorted_comps.begin(), sorted_comps.end(),
                  [](const auto& a, const auto& b) {
                      return a.second.size() > b.second.size();
                  });

        // ── ۵. ساخت JSON خروجی ───────────────────────────────

        std::ostringstream json;
        json << "{"
             << "\"total_components\":" << sorted_comps.size()
             << ",\"total_nodes\":" << M
             << ",\"largest_component_size\":" << sorted_comps[0].second.size()
             << ",\"components\":[";

        // فقط ۵۰ component اول در خروجی (جلوگیری از JSON خیلی بزرگ)
        const size_t MAX_COMP = 50;
        for (size_t ci = 0;
             ci < std::min(sorted_comps.size(), MAX_COMP); ++ci) {
            if (ci) json << ",";
            const auto& comp_nodes = sorted_comps[ci].second;
            json << "{\"id\":" << ci
                 << ",\"size\":" << comp_nodes.size()
                 << ",\"members\":[";

            // فقط ۱۰ عضو اول هر component
            const size_t MAX_MEMBERS = 10;
            for (size_t ni = 0;
                 ni < std::min(comp_nodes.size(), MAX_MEMBERS); ++ni) {
                if (ni) json << ",";
                json << "\"" << snapshot.extId(comp_nodes[ni]) << "\"";
            }
            if (comp_nodes.size() > MAX_MEMBERS)
                json << ",\"...(" << (comp_nodes.size() - MAX_MEMBERS) << " more)\"";

            json << "]}";
        }
        if (sorted_comps.size() > MAX_COMP)
            json << ",{\"note\":\"" << (sorted_comps.size() - MAX_COMP)
                 << " more components not shown\"}";

        json << "]}";

        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        return AlgoResult{true, "", json.str(), ms};
    }
};

AlgoResult runConnectedComponents(GraphManager& manager,
                                  const std::string& graph_name,
                                  const std::vector<ExtId>& params) {
    ConnectedComponents algo;
    auto handle = manager.submitJob(graph_name, algo, params);
    return handle.result();
}

} // namespace algorithms
} // namespace graph
} // namespace nexora
