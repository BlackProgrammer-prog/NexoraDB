/**
 * @file graph/algorithms/MutualFriends.cpp
 * @brief الگوریتم MutualFriends — LockAlgorithm (نوع سبک)
 *
 * ═══════════════════════════════════════════════════════════════
 * راهنما برای تیم الگوریتم — LockAlgorithm چیست؟
 * ═══════════════════════════════════════════════════════════════
 *
 * وقتی از LockAlgorithm استفاده می‌کنی:
 *   1. GraphManager یک shared_lock روی LiveGraph می‌گیرد
 *   2. تابع run() صدا زده می‌شود — گراف read-only است
 *   3. نتیجه برمی‌گردد → lock آزاد می‌شود
 *
 * در طول اجرا:
 *   ✅ graph.neighbors() — مجاز
 *   ✅ graph.hasEdge()   — مجاز
 *   ✅ graph.getDenseId() — مجاز
 *   ✅ graph.forEachOutEdge() — مجاز
 *   ❌ graph.addNode()   — ممنوع (write است)
 *   ❌ graph.addEdge()   — ممنوع
 *   ❌ DocEngine CRUD    — ممنوع (فقط گراف)
 *
 * کِی از LockAlgorithm استفاده کن؟
 *   • زیر ۲۰۰ms اجرا می‌شود
 *   • روی یک یا چند node خاص کار می‌کند (نه کل گراف)
 *   • کاربر منتظر نتیجه می‌ماند (blocking)
 *
 * ═══════════════════════════════════════════════════════════════
 * این الگوریتم: MutualFriends (دوستان مشترک)
 *   روش: Two-Pointer Intersection روی Sorted adjacency list
 *   پیچیدگی: O(deg_u + deg_v) — بهتر از hash set
 *   دلیل: adj list ما مرتب است (ChunkedSortedVector)
 * ═══════════════════════════════════════════════════════════════
 *
 * چطور صدا می‌شود (از Python):
 *   result = gm.run_lock("social_graph", "MutualFriends", ["u1","u2"], limit=100)
 *   # result.success, result.result_json
 */

#include "BuiltinAlgorithms.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <vector>
namespace nexora {
namespace graph {
namespace algorithms {

/**
 * @class MutualFriends
 * @brief دوستان مشترک دو کاربر — Two-Pointer روی Sorted Adjacency
 *
 * @section نحوه دریافت داده از گراف
 *
 * ما از این متدهای LiveGraph استفاده می‌کنیم:
 *
 *   graph.getDenseId(ext_id)
 *     → تبدیل "_id" سند به DenseId داخلی
 *
 *   graph.forEachOutEdge(dense_id, callback)
 *     → iterate روی یال‌های خروجی یک node
 *     → callback: fn(AdjEntry&) → bool
 *     → AdjEntry.neighbor = DenseId همسایه
 *     → AdjEntry.type_id  = نوع یال (برای فیلتر)
 *
 *   graph.getExtId(dense_id)
 *     → برگشت به string "_id" برای خروجی
 *
 *   graph.getEdgeTypeId("FOLLOWS")
 *     → گرفتن TypeId برای فیلتر نوع یال
 *
 *   graph.activeNodeCount() / activeEdgeCount()
 *     → آمار کلی (بدون iterate)
 */
class MutualFriends : public LockAlgorithm {
public:
    std::string name() const override { return "MutualFriends"; }

    /**
     * @param graph   LiveGraph (read-only — shared_lock فعال است)
     * @param params  ["user1_id", "user2_id", optional:"edge_type"]
     *                مثال: ["u1", "u2"] یا ["u1", "u2", "FOLLOWS"]
     */
    AlgoResult run(const LiveGraph& graph,
                   const std::vector<ExtId>& params) override
    {
        auto t0 = std::chrono::steady_clock::now();

        // ── ۱. اعتبارسنجی پارامترها ──────────────────────────
        if (params.size() < 2) {
            return AlgoResult{false, "Need 2 user IDs: [user1, user2]", "", 0.0};
        }

        const ExtId& u1_ext = params[0];
        const ExtId& u2_ext = params[1];

        // ── ۲. تبدیل ExtId به DenseId ───────────────────────
        // DenseId = index پیوسته در vector داخلی
        // اگر کاربر وجود نداشت kInvalidDenseId برمی‌گردد
        DenseId u1 = graph.getDenseId(u1_ext);
        DenseId u2 = graph.getDenseId(u2_ext);

        if (u1 == kInvalidDenseId)
            return AlgoResult{false, "User not found: " + u1_ext, "", 0.0};
        if (u2 == kInvalidDenseId)
            return AlgoResult{false, "User not found: " + u2_ext, "", 0.0};

        // ── ۳. گرفتن TypeId برای فیلتر (اختیاری) ───────────
        // اگر params[2] داده شد، فقط آن نوع یال را نگاه می‌کنیم
        TypeId filter_type = kInvalidTypeId;  // kInvalidTypeId = همه انواع
        if (params.size() >= 3 && !params[2].empty()) {
            auto tid = graph.getEdgeTypeId(params[2]);
            if (!tid) {
                return AlgoResult{false, "Edge type not found: " + params[2], "", 0.0};
            }
            filter_type = *tid;
        }

        // ── ۴. جمع‌آوری neighbors هر کاربر ──────────────────
        // از forEachOutEdge استفاده می‌کنیم چون:
        //   • GIL-free است
        //   • callback-based = بدون allocation اضافی
        //   • adj list مرتب است = آماده برای two-pointer

        std::vector<DenseId> friends_u1, friends_u2;

        // حجم تقریبی برای reserve (از NodeRecord که از قبل cache شده)
        auto node_u1 = graph.getNode(u1);
        auto node_u2 = graph.getNode(u2);
        if (node_u1) friends_u1.reserve(node_u1->out_degree);
        if (node_u2) friends_u2.reserve(node_u2->out_degree);

        // collect neighbors u1
        graph.forEachOutEdge(u1, [&](const AdjEntry& e) -> bool {
            if (filter_type == kInvalidTypeId || e.type_id == filter_type) {
                friends_u1.push_back(e.neighbor);
            }
            return true;  // ادامه بده
        });

        // collect neighbors u2
        graph.forEachOutEdge(u2, [&](const AdjEntry& e) -> bool {
            if (filter_type == kInvalidTypeId || e.type_id == filter_type) {
                friends_u2.push_back(e.neighbor);
            }
            return true;
        });

        // ── ۵. Two-Pointer Intersection ───────────────────────
        // adj list قبلاً مرتب است (ChunkedSortedVector)
        // پس نیازی به sort نداریم — مستقیم two-pointer می‌زنیم
        //
        // اگر مرتب نبود: std::sort(f1), std::sort(f2) — O(n log n)
        // با مرتب بودن: O(deg_u + deg_v) — بهتر از hash set

        std::vector<DenseId> mutual;

        size_t i = 0, j = 0;
        while (i < friends_u1.size() && j < friends_u2.size()) {
            if (friends_u1[i] == friends_u2[j]) {
                // دوست مشترک پیدا شد
                // فیلتر: نه u1 و نه u2 خودشان در نتیجه نباشند
                if (friends_u1[i] != u1 && friends_u1[i] != u2) {
                    mutual.push_back(friends_u1[i]);
                }
                ++i; ++j;
            } else if (friends_u1[i] < friends_u2[j]) {
                ++i;
            } else {
                ++j;
            }
        }

        // ── ۶. ساخت JSON خروجی ───────────────────────────────
        // DenseId را به ExtId برمی‌گردانیم (همان "_id" سند)
        std::ostringstream json;
        json << "{\"mutual_friends\":[";
        for (size_t k = 0; k < mutual.size(); ++k) {
            if (k) json << ",";
            json << "\"" << graph.getExtId(mutual[k]) << "\"";
        }
        json << "],\"count\":" << mutual.size()
             << ",\"u1\":\"" << u1_ext << "\""
             << ",\"u2\":\"" << u2_ext << "\"}";

        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        return AlgoResult{true, "", json.str(), ms};
    }
};

AlgoResult runMutualFriends(GraphManager& manager,
                            const std::string& graph_name,
                            const std::vector<ExtId>& params) {
    MutualFriends algo;
    return manager.runLock(graph_name, algo, params);
}

} // namespace algorithms
} // namespace graph
} // namespace nexora
