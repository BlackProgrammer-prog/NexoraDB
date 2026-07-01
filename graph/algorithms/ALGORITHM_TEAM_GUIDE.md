# NexoraDB — راهنمای تیم الگوریتم

> **مخاطب:** تیم الگوریتم‌نویسی C++  
> **پیش‌نیاز:** آشنایی با C++17/20 و ساختار داده‌های گراف  
> **زمان مطالعه:** ۲۰ دقیقه

---

## فهرست

1. [شروع سریع](#۱-شروع-سریع)
2. [دو نوع الگوریتم](#۲-دو-نوع-الگوریتم)
3. [LockAlgorithm — الگوریتم سبک](#۳-lockalgorithm--الگوریتم-سبک)
4. [JobAlgorithm — الگوریتم سنگین](#۴-jobalgorithm--الگوریتم-سنگین)
5. [متدهای LiveGraph](#۵-متدهای-livegraph)
6. [متدهای StaticGraph](#۶-متدهای-staticgraph)
7. [ساختارهای داده](#۷-ساختارهای-داده)
8. [اجرا از GraphManager](#۸-اجرا-از-graphmanager)
9. [قوانین مهم](#۹-قوانین-مهم)
10. [چک‌لیست تحویل](#۱۰-چکلیست-تحویل)

---

## ۱. شروع سریع

**سه قدم برای نوشتن الگوریتم:**

```
۱. یک فایل .cpp در  graph/algorithms/  بساز
۲. از LockAlgorithm یا JobAlgorithm ارث ببر
۳. تابع run() را پیاده کن
```

**کمترین کدی که کار می‌کند:**

```cpp
// graph/algorithms/MyAlgo.cpp
#include "AlgorithmBase.h"

namespace nexora::graph::algorithms {

class MyAlgo : public LockAlgorithm {
public:
    std::string name() const override { return "MyAlgo"; }

    AlgoResult run(const LiveGraph& graph,
                   const std::vector<ExtId>& params) override {
        // کد شما اینجا
        return AlgoResult{true, "", "{\"result\":\"ok\"}", 0.0};
    }
};

} // namespace
```

---

## ۲. دو نوع الگوریتم

```
┌─────────────────────────────┬──────────────────────────────┐
│      LockAlgorithm          │       JobAlgorithm           │
├─────────────────────────────┼──────────────────────────────┤
│  روی LiveGraph              │  روی StaticGraph (snapshot)  │
│  Blocking — کاربر منتظر    │  Async — background thread   │
│  زیر ۲۰۰ms                 │  بدون محدودیت زمان           │
│  یک یا چند node خاص        │  کل گراف                     │
│  shared_lock فعال است       │  هیچ lock ای نیست            │
└─────────────────────────────┴──────────────────────────────┘
```

**چه وقت کدام را انتخاب کنم؟**

| سوال | جواب |
|------|------|
| روی کل گراف کار می‌کند؟ | → JobAlgorithm |
| بیشتر از ۲۰۰ms طول می‌کشد؟ | → JobAlgorithm |
| فقط روی یک node و همسایه‌هایش؟ | → LockAlgorithm |
| کاربر بلادرنگ نتیجه می‌خواهد؟ | → LockAlgorithm |

---

## ۳. LockAlgorithm — الگوریتم سبک

### ساختار پایه

```cpp
#include "AlgorithmBase.h"
#include <chrono>
#include <sstream>

namespace nexora::graph::algorithms {

class MyLockAlgo : public LockAlgorithm {
public:
    // نام الگوریتم — برای لاگ و Python API
    std::string name() const override { return "MyLockAlgo"; }

    AlgoResult run(const LiveGraph& graph,
                   const std::vector<ExtId>& params) override {

        auto t0 = std::chrono::steady_clock::now();

        // ── کد اصلی شما اینجا ──

        double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();

        return AlgoResult{
            true,        // success
            "",          // error_msg (خالی = موفق)
            "{...}",     // result_json
            ms           // elapsed_ms
        };
    }
};

} // namespace
```

### مثال کامل — MutualFriends

> **هدف:** پیدا کردن دوستان مشترک دو کاربر  
> **روش:** Two-Pointer Intersection روی Sorted adjacency list  
> **پیچیدگی:** O(deg_u + deg_v)

```cpp
#include "AlgorithmBase.h"
#include <algorithm>
#include <chrono>
#include <sstream>

namespace nexora::graph::algorithms {

class MutualFriends : public LockAlgorithm {
public:
    std::string name() const override { return "MutualFriends"; }

    // params = ["user1_id", "user2_id"]
    // مثال:  ["u1", "u2"]
    AlgoResult run(const LiveGraph& graph,
                   const std::vector<ExtId>& params) override {

        auto t0 = std::chrono::steady_clock::now();

        // ── ۱. بررسی params ──
        if (params.size() < 2)
            return AlgoResult{false, "Need 2 user IDs"};

        // ── ۲. ExtId → DenseId ──
        DenseId u1 = graph.getDenseId(params[0]);
        DenseId u2 = graph.getDenseId(params[1]);

        if (u1 == kInvalidDenseId)
            return AlgoResult{false, "Not found: " + params[0]};
        if (u2 == kInvalidDenseId)
            return AlgoResult{false, "Not found: " + params[1]};

        // ── ۳. گرفتن TypeId (اختیاری) ──
        TypeId follow_type = kInvalidTypeId; // همه انواع
        auto tid = graph.getEdgeTypeId("FOLLOWS");
        if (tid) follow_type = *tid;

        // ── ۴. جمع‌آوری neighbors ──
        std::vector<DenseId> f1, f2;

        graph.forEachOutEdge(u1, [&](const AdjEntry& e) -> bool {
            if (follow_type == kInvalidTypeId || e.type_id == follow_type)
                f1.push_back(e.neighbor);
            return true; // ادامه بده
        });

        graph.forEachOutEdge(u2, [&](const AdjEntry& e) -> bool {
            if (follow_type == kInvalidTypeId || e.type_id == follow_type)
                f2.push_back(e.neighbor);
            return true;
        });

        // ── ۵. Two-Pointer Intersection ──
        // adj list مرتب است → نیازی به sort نیست
        std::vector<DenseId> mutual;
        size_t i = 0, j = 0;
        while (i < f1.size() && j < f2.size()) {
            if      (f1[i] == f2[j]) { mutual.push_back(f1[i]); ++i; ++j; }
            else if (f1[i]  < f2[j]) { ++i; }
            else                     { ++j; }
        }

        // ── ۶. ساخت JSON ──
        std::ostringstream json;
        json << "{\"mutual_friends\":[";
        for (size_t k = 0; k < mutual.size(); ++k) {
            if (k) json << ",";
            json << "\"" << graph.getExtId(mutual[k]) << "\"";
        }
        json << "],\"count\":" << mutual.size() << "}";

        double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();

        return AlgoResult{true, "", json.str(), ms};
    }
};

} // namespace
```

---

## ۴. JobAlgorithm — الگوریتم سنگین

### ساختار پایه

```cpp
#include "AlgorithmBase.h"
#include <chrono>

namespace nexora::graph::algorithms {

class MyJobAlgo : public JobAlgorithm {
public:
    std::string name() const override { return "MyJobAlgo"; }

    AlgoResult run(const StaticGraph& snapshot,
                   const std::vector<ExtId>& params) override {

        auto t0 = std::chrono::steady_clock::now();

        // ── کد اصلی شما اینجا ──
        // snapshot ایمن است — هیچ lock نیاز نیست
        // LiveGraph می‌تواند همزمان تغییر کند

        double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();

        return AlgoResult{true, "", "{...}", ms};
    }
};

} // namespace
```

### مثال کامل — ConnectedComponents

> **هدف:** پیدا کردن مؤلفه‌های متصل (Connected Components)  
> **روش:** Union-Find (DSU) با Path Compression  
> **پیچیدگی:** O(E·α(V)) ≈ O(E)  
> **چرا DSU؟** یک pass روی edges کافی است — N بار BFS لازم نیست

```cpp
#include "AlgorithmBase.h"
#include <algorithm>
#include <chrono>
#include <numeric>
#include <sstream>
#include <unordered_map>

namespace nexora::graph::algorithms {

class ConnectedComponents : public JobAlgorithm {
public:
    std::string name() const override { return "ConnectedComponents"; }

    // params = [] یا ["User"] (فیلتر نوع node)
    AlgoResult run(const StaticGraph& snapshot,
                   const std::vector<ExtId>& params) override {

        auto t0 = std::chrono::steady_clock::now();

        // ── ۱. جمع‌آوری nodes ──
        std::vector<DenseId> all_nodes;
        all_nodes.reserve(snapshot.nodeCount());

        snapshot.forEachNode([&](DenseId id, TypeId tid) -> bool {
            // فیلتر نوع اگر params داده شد
            if (!params.empty() && !params[0].empty())
                if (snapshot.nodeTypeName(tid) != params[0]) return true;
            all_nodes.push_back(id);
            return true;
        });

        // ── ۲. ساخت DSU ──
        std::unordered_map<DenseId, uint32_t> idx;
        for (uint32_t i = 0; i < all_nodes.size(); ++i)
            idx[all_nodes[i]] = i;

        const uint32_t N = all_nodes.size();
        std::vector<uint32_t> parent(N), rank(N, 0);
        std::iota(parent.begin(), parent.end(), 0);

        // find با path compression
        std::function<uint32_t(uint32_t)> find = [&](uint32_t x) -> uint32_t {
            if (parent[x] != x) parent[x] = find(parent[x]);
            return parent[x];
        };

        // union by rank
        auto unite = [&](uint32_t a, uint32_t b) {
            uint32_t ra = find(a), rb = find(b);
            if (ra == rb) return;
            if (rank[ra] < rank[rb]) std::swap(ra, rb);
            parent[rb] = ra;
            if (rank[ra] == rank[rb]) ++rank[ra];
        };

        // ── ۳. یک pass روی edges ──
        snapshot.forEachEdge(
            [&](EdgeId, DenseId src, DenseId dst, TypeId) -> bool {
                auto it_s = idx.find(src), it_d = idx.find(dst);
                if (it_s != idx.end() && it_d != idx.end())
                    unite(it_s->second, it_d->second);
                return true;
            });

        // ── ۴. گروه‌بندی نتیجه ──
        std::unordered_map<uint32_t, std::vector<ExtId>> comps;
        for (uint32_t i = 0; i < N; ++i)
            comps[find(i)].push_back(snapshot.extId(all_nodes[i]));

        // مرتب از بزرگ به کوچک
        std::vector<std::vector<ExtId>> sorted;
        for (auto& [_, members] : comps) sorted.push_back(std::move(members));
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b){ return a.size() > b.size(); });

        // ── ۵. JSON ──
        std::ostringstream json;
        json << "{\"total_components\":" << sorted.size()
             << ",\"components\":[";
        for (size_t ci = 0; ci < sorted.size(); ++ci) {
            if (ci) json << ",";
            json << "{\"size\":" << sorted[ci].size() << ",\"members\":[";
            for (size_t ni = 0; ni < std::min(sorted[ci].size(), size_t{5}); ++ni) {
                if (ni) json << ",";
                json << "\"" << sorted[ci][ni] << "\"";
            }
            json << "]}";
        }
        json << "]}";

        double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();

        return AlgoResult{true, "", json.str(), ms};
    }
};

} // namespace
```

---

## ۵. متدهای LiveGraph

> این متدها را در **LockAlgorithm** استفاده کنید.

### تبدیل ID

```cpp
// ExtId ("u1") → DenseId (عدد داخلی)
DenseId id = graph.getDenseId("u1");
if (id == kInvalidDenseId) { /* وجود ندارد */ }

// DenseId → ExtId
ExtId ext = graph.getExtId(id); // "u1"
```

### اطلاعات node

```cpp
// اطلاعات کامل یک node
auto node = graph.getNode("u1");   // یا getNode(dense_id)
if (node) {
    node->dense_id;    // DenseId
    node->external_id; // "_id" سند → "u1"
    node->type_name;   // "User"
    node->out_degree;  // تعداد یال خروجی (cache شده)
    node->in_degree;   // تعداد یال ورودی  (cache شده)
}

// بررسی وجود
bool exists = graph.hasNode("u1");
```

### گرفتن همسایه‌ها

```cpp
// روش ۱ — لیست DenseId (سریع‌تر)
auto nbrs = graph.neighbors(uid, Direction::Out);
// Direction::Out  → یال‌های خروجی
// Direction::In   → یال‌های ورودی
// Direction::Both → هر دو

// روش ۲ — با فیلتر type
auto tid = graph.getEdgeTypeId("FOLLOWS");
auto nbrs = graph.neighbors(uid, Direction::Out, *tid, 100); // limit=100

// روش ۳ — لیست ExtId
auto nbrs = graph.neighborsExt("u1", Direction::Out, "FOLLOWS", 50);
// خروجی: vector<string> = ["u2", "u3"]
```

### iterate روی edges

```cpp
// iterate یال‌های خروجی یک node
graph.forEachOutEdge(uid, [&](const AdjEntry& e) -> bool {
    DenseId neighbor = e.neighbor;  // DenseId همسایه
    TypeId  etype    = e.type_id;   // نوع یال
    EdgeId  eid      = e.edge_id;   // id یال
    return true;  // false = زودتر توقف کن
});

// iterate یال‌های ورودی
graph.forEachInEdge(uid, [&](const AdjEntry& e) -> bool {
    // e.neighbor = کسی که به uid یال دارد
    return true;
});

// iterate همه nodes
graph.forEachNode([&](DenseId id, const NodeRecord& rec) -> bool {
    rec.out_degree; // degree بدون شمارش — O(1)
    rec.in_degree;
    rec.type_id;
    return true;
});
```

### بررسی یال

```cpp
bool connected = graph.hasEdge("u1", "u2", "FOLLOWS");
```

### آمار

```cpp
uint64_t nodes = graph.activeNodeCount(); // O(1)
uint64_t edges = graph.activeEdgeCount(); // O(1)
```

---

## ۶. متدهای StaticGraph

> این متدها را در **JobAlgorithm** استفاده کنید.

### اطلاعات کلی

```cpp
uint64_t n = snapshot.nodeCount(); // تعداد nodes
uint64_t e = snapshot.edgeCount(); // تعداد edges
```

### تبدیل ID

```cpp
DenseId id  = snapshot.denseId("u1"); // ExtId → DenseId
ExtId   ext = snapshot.extId(id);     // DenseId → ExtId
```

### اطلاعات node

```cpp
TypeId   tid     = snapshot.nodeType(id);
uint64_t out_deg = snapshot.outDegree(id);
uint64_t in_deg  = snapshot.inDegree(id);
std::string type = snapshot.nodeTypeName(tid); // "User"
```

### iterate

```cpp
// همه nodes
snapshot.forEachNode([&](DenseId id, TypeId tid) -> bool {
    std::string ext  = snapshot.extId(id);
    std::string type = snapshot.nodeTypeName(tid);
    return true;
});

// همه edges — برای الگوریتم‌های روی کل گراف
snapshot.forEachEdge(
    [&](EdgeId eid, DenseId src, DenseId dst, TypeId tid) -> bool {
        // src → dst
        return true;
    });

// neighbors بدون allocation (سریع‌ترین روش)
snapshot.forEachNeighbor(id, Direction::Out,
    [&](DenseId neighbor, TypeId edge_type) -> bool {
        return true;
    });

// neighbors با allocation
auto nbrs = snapshot.neighbors(id, Direction::Out);
```

### export برای GPU / ML

```cpp
// COO format — برای PyTorch Geometric
GraphExportOptions opts;
opts.remapNodeIdsToContiguous = true; // index از 0 شروع شود
CooGraph coo = snapshot.exportCOO(opts);
// coo.src[i] → coo.dst[i]  = یک edge
// coo.originalNodeIds[i]   = DenseId اصلی node i

// CSR format — برای GPU BFS
CsrGraph csr = snapshot.exportCSR(opts);
// csr.rowPtr[i], csr.rowPtr[i+1] = بازه edges node i
```

---

## ۷. ساختارهای داده

### انواع ID

```cpp
using DenseId = uint64_t;  // index داخلی — سریع‌ترین دسترسی
using ExtId   = std::string; // "_id" سند در RocksDB
using TypeId  = uint32_t;  // نوع node یا edge
using EdgeId  = uint64_t;  // id یال

// مقادیر نامعتبر
kInvalidDenseId  // UINT64_MAX — "وجود ندارد"
kInvalidTypeId   // UINT32_MAX — "همه انواع"
```

### AdjEntry — یک یال در adjacency list

```cpp
struct AdjEntry {
    DenseId neighbor; // DenseId همسایه
    EdgeId  edge_id;  // id یال
    TypeId  type_id;  // نوع یال ("FOLLOWS"=0, "LIKES"=1, ...)
};
// نکته مهم: AdjEntry.neighbor مرتب است — Two-Pointer کار می‌کند!
```

### Direction

```cpp
enum class Direction {
    Out,   // یال‌های خروجی از node
    In,    // یال‌های ورودی به node
    Both   // هر دو
};
```

### AlgoResult

```cpp
struct AlgoResult {
    bool        success;     // true = موفق
    std::string error_msg;   // پیام خطا (اگر success=false)
    std::string result_json; // نتیجه JSON
    double      elapsed_ms;  // زمان اجرا
};
```

---

## ۸. اجرا از GraphManager

### LockAlgorithm (C++)

```cpp
#include "graph/GraphManager.h"
#include "graph/algorithms/MutualFriends.cpp"

// ── اجرا ──
MutualFriends algo;
AlgoResult result = gm.runLock("social_graph", algo, {"u1", "u2"});

if (result.success) {
    std::cout << result.result_json; // {"mutual_friends":["u3"],"count":1}
    std::cout << result.elapsed_ms;  // مثلاً 0.45
} else {
    std::cerr << result.error_msg;
}
```

### JobAlgorithm (C++)

```cpp
#include "graph/algorithms/ConnectedComponents.cpp"

// ── submit و ادامه کار ──
ConnectedComponents algo;
JobHandle handle = gm.submitJob("social_graph", algo, {});

// می‌توانید کار دیگری انجام دهید...
std::this_thread::sleep_for(std::chrono::milliseconds(100));

// ── گرفتن نتیجه (blocking) ──
AlgoResult result = handle.result();
std::cout << result.result_json;
```

### از Python

```python
# بعد از پیاده‌سازی در C++، از Python قابل دسترس خواهد بود
# (نیاز به binding دارد — با تیم Storage هماهنگ کنید)
```

---

## ۹. قوانین مهم

### ✅ مجاز

```cpp
// LockAlgorithm — فقط read از graph
graph.getDenseId()
graph.getExtId()
graph.getNode()
graph.hasNode()
graph.hasEdge()
graph.neighbors()
graph.neighborsExt()
graph.forEachOutEdge()
graph.forEachInEdge()
graph.forEachNode()
graph.forEachEdge()
graph.getEdgeTypeId()
graph.activeNodeCount()
graph.activeEdgeCount()

// JobAlgorithm — همه متدهای StaticGraph
snapshot.forEachNode()
snapshot.forEachEdge()
snapshot.forEachNeighbor()
snapshot.neighbors()
snapshot.nodeCount()
snapshot.edgeCount()
snapshot.extId()
snapshot.denseId()
snapshot.outDegree()
snapshot.inDegree()
snapshot.exportCOO()
snapshot.exportCSR()
```

### ❌ ممنوع

```cpp
// در LockAlgorithm — write ممنوع است
graph.addNode(...)    // ❌ Segfault یا deadlock
graph.addEdge(...)    // ❌
graph.removeNode(...) // ❌

// در JobAlgorithm — LiveGraph ممنوع است
LiveGraph* live = ...;  // ❌ فقط snapshot مجاز است
DocEngine engine = ...; // ❌

// در هر دو — return false را فراموش نکنید
graph.forEachOutEdge(uid, [&](const AdjEntry& e) {
    // ❌ باید bool برگردانید
});
graph.forEachOutEdge(uid, [&](const AdjEntry& e) -> bool {
    return true; // ✅
});
```

### نکته مهم: degree cache شده است

```cpp
// ❌ اشتباه — شمردن دستی
int count = 0;
graph.forEachOutEdge(uid, [&](const AdjEntry&) -> bool {
    ++count;
    return true;
});

// ✅ درست — O(1) از NodeRecord
auto node = graph.getNode(uid);
uint64_t deg = node->out_degree; // از قبل cache شده
```

---

## ۱۰. چک‌لیست تحویل

قبل از تحویل هر الگوریتم این موارد را بررسی کنید:

- [ ] فایل در `graph/algorithms/` قرار دارد
- [ ] از `AlgorithmBase.h` include شده (نه مستقیم از LiveGraph)
- [ ] `name()` یک نام یکتا برمی‌گرداند
- [ ] پارامترهای ورودی در ابتدای `run()` validate شده‌اند
- [ ] اگر node پیدا نشد `AlgoResult{false, "پیام خطا"}` برمی‌گردد
- [ ] خروجی یک JSON معتبر است
- [ ] `elapsed_ms` محاسبه شده است
- [ ] در LockAlgorithm: هیچ write‌ای نشده
- [ ] در JobAlgorithm: فقط از `snapshot` استفاده شده
- [ ] نام فایل به `graph/CMakeLists.txt` اضافه شده:

```cmake
set(ALGO_USER_SOURCES
    algorithms/MutualFriends.cpp
    algorithms/ConnectedComponents.cpp
    algorithms/YourAlgo.cpp    # ← اینجا
)
```

---

## جدول الگوریتم‌ها

| نام | نوع | روش | پیچیدگی | پارامترها |
|-----|-----|-----|----------|-----------|
| `MutualFriends` | Lock | Two-Pointer | O(deg_u + deg_v) | `["u1","u2"]` |
| `AreConnected` | Lock | Bidirectional BFS | O(b^(d/2)) | `["u1","u2"]` |
| `ShortestPath` | Lock | Bidir BFS + Parents | O(b^(d/2)) | `["u1","u2"]` |
| `FriendSuggestion` | Lock | FoF + Jaccard | O(deg × avg_deg) | `["u1", limit]` |
| `MostConnected` | Lock | Degree Cache | O(1) / O(V) | `[limit]` |
| `NetworkStats` | Lock | Incremental Counter | O(1) | `[]` |
| `ConnectedComponents` | Job | DSU | O(E·α(V)) | `[]` یا `["User"]` |
| `AllDistances` | Job | BFS-SSSP | O(V+E) | `["src_id"]` |

---

*NexoraDB Algorithm Team Guide — v1.0*
