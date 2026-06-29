//
// Created by HOME on 6/19/2026.
//

#ifndef GITIGNORE_GRAPHMANAGER_H
#define GITIGNORE_GRAPHMANAGER_H


#pragma once

/**
 * @file graph/GraphManager.h
 * @brief مرکز کنترل کل GraphEngine — اتصال DocEngine به LiveGraph/StaticGraph
 *
 * @details
 * GraphManager تنها نقطه ارتباط بین DocEngine و GraphEngine است.
 *
 * ═══════════════════════════════════════════════════════════════
 * پاسخ به سوال "چرا DocEngine و GraphEngine ارتباط ندارند؟"
 * ═══════════════════════════════════════════════════════════════
 *
 * DocEngine و LiveGraph عمداً از هم جدا هستند چون:
 *  - DocEngine فقط BSON در RocksDB ذخیره/خواند می‌کند
 *  - LiveGraph فقط ساختار RAM گراف را نگه می‌دارد
 *  - GraphManager این دو را به هم وصل می‌کند
 *
 * این جداسازی اجازه می‌دهد:
 *  ✅ DocEngine مستقل از Graph تست شود
 *  ✅ Graph بدون DocEngine rebuild شود (از فایل‌های .nex/.nexr)
 *  ✅ تیم الگوریتم فقط با StaticGraph کار کند
 *
 * ═══════════════════════════════════════════════════════════════
 * جریان BuildGraph (برای LIVE graph):
 * ═══════════════════════════════════════════════════════════════
 *
 * 1. GraphManager::buildGraph(def)
 *    ├── برای هر NodeMappingDef:
 *    │   └── DocEngine::IterateCollection(collection, callback)
 *    │       └── callback: ExtractField(key_path) → addNode(ext_id, type)
 *    │                     ExtractField(filter) → skip if not match
 *    └── برای هر EdgeMappingDef:
 *        ├── روش ۱ (collection جدا): IterateCollection → addEdge(src,dst,type)
 *        ├── روش ۲ (FK در سند):      IterateCollection → ExtractField → addEdge
 *        └── روش ۳ (UNWIND array):   IterateCollection → foreach array item → addEdge
 *
 * ═══════════════════════════════════════════════════════════════
 * جریان Live Update (وقتی DocEngine تغییر می‌کند):
 * ═══════════════════════════════════════════════════════════════
 *
 * DocEngine::InsertOne("users", bson)
 *   → DocEngine فراخوانی می‌کند: GraphManager::onDocumentInserted("users", bson)
 *   → GraphManager بررسی می‌کند: کدام graph باید update شود؟
 *   → LiveGraph::addNode یا addEdge فراخوانی می‌شود (incremental — نه rebuild)
 *
 * ═══════════════════════════════════════════════════════════════
 * جریان StaticGraph برای تیم الگوریتم:
 * ═══════════════════════════════════════════════════════════════
 *
 * JobRunner::submit(algo, graph_name, params)
 *   → GraphManager::createSnapshot(graph_name) → StaticGraph
 *   → background thread: algo.run(static_graph, params)
 *   → StaticGraph آزاد می‌شود
 *   → نتیجه به caller برمی‌گردد
 */

#include "Graphtypes.h"
#include "Graphstorage.h"
#include "Graphwal.h"
#include "GraphIdStore.h"
#include "Livegraph.h"
#include "StaticGraph.h"
#include "algorithms/AlgorithmBase.h"

// DocEngine (برای IterateCollection، GetForeignKeys، ExtractField)
#include "../core/DocEngine.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace nexora {
    namespace graph {

// forward
        class JobRunner;

/**
 * @struct BuildResult
 * @brief نتیجه build یک گراف
 */
        struct BuildResult {
            bool        success    = false;
            std::string error_msg;
            uint64_t    nodes_built = 0;
            uint64_t    edges_built = 0;
            double      elapsed_ms  = 0.0;
        };

/**
 * @class JobHandle
 * @brief Handle برگشتی از GraphManager::submitJob برای گرفتن نتیجه async
 */
        class JobHandle {
        public:
            JobHandle() = default;
            explicit JobHandle(std::future<algorithms::AlgoResult>&& future)
                    : future_(std::move(future)) {}

            JobHandle(const JobHandle&)            = delete;
            JobHandle& operator=(const JobHandle&) = delete;
            JobHandle(JobHandle&&) noexcept        = default;
            JobHandle& operator=(JobHandle&&) noexcept = default;

            bool valid() const noexcept { return future_.valid(); }

            algorithms::AlgoResult result() {
                if (!future_.valid())
                    return {false, "Invalid job handle", "", 0.0};
                return future_.get();
            }

            template <class Rep, class Period>
            std::future_status waitFor(
                    const std::chrono::duration<Rep, Period>& timeout) const {
                if (!future_.valid()) return std::future_status::deferred;
                return future_.wait_for(timeout);
            }

        private:
            mutable std::future<algorithms::AlgoResult> future_;
        };

/**
 * @struct GraphHandle
 * @brief اطلاعات runtime یک گراف
 */
        struct GraphHandle {
            GraphDefinition             definition;
            std::unique_ptr<GraphStorage> storage;
            std::unique_ptr<GraphWAL>     wal;
            std::unique_ptr<GraphIdStore> id_store;
            std::unique_ptr<LiveGraph>    live_graph;  ///< null اگر STATIC باشد
            mutable std::shared_mutex     rw_mutex;    ///< محافظت از live_graph
            bool                          built = false;
        };

/**
 * @class GraphManager
 * @brief Orchestrator — اتصال DocEngine به GraphEngine
 *
 * @section استفاده برای هر تیم:
 *
 * **تیم Storage (شما):**
 *   - هنگام InsertOne/UpdateById/DeleteById در DocEngine، باید
 *     GraphManager::onDocumentInserted/Updated/Deleted فراخوانی شود
 *   - این کار در NexoraDB wrapper انجام می‌شود
 *
 * **تیم Parser:**
 *   - createGraph() → defineNodeMapping() → defineEdgeMapping() → buildGraph()
 *   - از طریق FastAPI فراخوانی می‌شود
 *
 * **تیم الگوریتم:**
 *   - getLiveGraph() برای LockAlgorithm
 *   - createSnapshot() برای JobAlgorithm
 *   - submitJob() برای اجرای async
 */
        class GraphManager {
        public:
            /**
             * @param doc_engine   اشاره‌گر به DocEngine (source of truth)
             * @param graph_dir    پوشه‌ای که .nex/.nexr/.nexl/.wal در آن ذخیره می‌شوند
             */
            explicit GraphManager(nexora::core::DocEngine* doc_engine,
                                  std::filesystem::path graph_dir = "./graph_data");
            ~GraphManager();

            // non-copyable
            GraphManager(const GraphManager&)            = delete;
            GraphManager& operator=(const GraphManager&) = delete;

            // ──────────────────────────────────────────────────────────
            // §1  Lifecycle — Startup و Shutdown
            // ──────────────────────────────────────────────────────────

            /**
             * @brief در startup دیتابیس فراخوانی می‌شود
             *
             * کارهایی که انجام می‌دهد:
             *  1. تعریف‌های ذخیره‌شده گراف از RocksDB بارگذاری می‌کند
             *  2. برای هر گراف با auto_build=true:
             *     - GraphIdStore را بارگذاری می‌کند (DenseId map از RocksDB)
             *     - فایل‌های .nex/.nexr را اسکن می‌کند
             *     - اگر meta clean نباشد → rebuild از DocEngine
             *     - WAL replay می‌کند
             *  3. LIVE graph‌ها آماده دریافت تغییر می‌شوند
             */
            bool startup();

            /**
             * @brief در shutdown دیتابیس فراخوانی می‌شود
             * WAL flush، فایل‌ها بسته می‌شوند
             */
            void shutdown();

            // ──────────────────────────────────────────────────────────
            // §2  تعریف گراف (Parser این‌ها را صدا می‌زند)
            // ──────────────────────────────────────────────────────────

            /**
             * @brief یک گراف جدید تعریف می‌کند
             * @param def   تعریف کامل گراف (شامل node/edge mappings)
             * @return true اگر موفق
             *
             * @note تعریف در RocksDB ذخیره می‌شود (persistent)
             */
            bool createGraph(const GraphDefinition& def);

            /**
             * @brief یک node mapping به گراف اضافه می‌کند
             *
             * NexoraQL: MAP NODE User FROM users KEY _id PROPERTIES username;
             */
            bool addNodeMapping(const std::string& graph_name,
                                const NodeMappingDef& mapping);

            /**
             * @brief یک edge mapping به گراف اضافه می‌کند
             *
             * NexoraQL: MAP EDGE FOLLOWS FROM follows SOURCE from_id AS User TARGET to_id AS User;
             */
            bool addEdgeMapping(const std::string& graph_name,
                                const EdgeMappingDef& mapping);

            /**
             * @brief یک گراف را حذف می‌کند (تعریف + فایل‌ها + RocksDB entries)
             */
            bool dropGraph(const std::string& graph_name);

            /**
             * @brief تعریف یک گراف را برمی‌گرداند
             */
            std::optional<GraphDefinition> getDefinition(const std::string& graph_name) const;

            /**
             * @brief لیست همه گراف‌ها
             */
            std::vector<std::string> listGraphs() const;

            // ──────────────────────────────────────────────────────────
            // §3  Build و Render
            // ──────────────────────────────────────────────────────────

            /**
             * @brief گراف را از DocEngine می‌سازد (روی فایل‌های .nex/.nexr می‌نویسد)
             *
             * @details
             * این تابع:
             *  1. meta state را Dirty می‌کند
             *  2. برای هر NodeMappingDef:
             *     DocEngine::IterateCollection → addNode → GraphStorage::writeNode
             *  3. برای هر EdgeMappingDef:
             *     DocEngine::IterateCollection → extract src/dst → addEdge
             *  4. meta state را Clean می‌کند
             *
             * NexoraQL: BUILD GRAPH social_graph;
             */
            BuildResult buildGraph(const std::string& graph_name);

            /**
             * @brief گراف را از فایل‌های .nex/.nexr در RAM بارگذاری می‌کند
             *
             * NexoraQL: RENDER GRAPH social_graph;
             */
            bool renderGraph(const std::string& graph_name);

            /**
             * @brief rebuild کامل گراف (build + render)
             *
             * NexoraQL: REFRESH GRAPH social_graph;
             */
            BuildResult refreshGraph(const std::string& graph_name);

            /**
             * @brief compaction فایل‌های دیسک در زمان خلوت
             *
             * NexoraQL: COMPACT GRAPH social_graph;
             */
            bool compactGraph(const std::string& graph_name);

            // ──────────────────────────────────────────────────────────
            // §4  دسترسی به LiveGraph (برای LockAlgorithm و traversal)
            // ──────────────────────────────────────────────────────────

            /**
             * @brief دسترسی read به LiveGraph
             * @return nullptr اگر گراف وجود نداشته باشد یا هنوز build نشده
             *
             * @note برای LockAlgorithm:
             * ```cpp
             * auto* g = manager.getLiveGraph("social_graph");
             * if (g) {
             *     auto friends = g->neighborsExt("u_001", Direction::Out, "FOLLOWS");
             * }
             * ```
             */
            const LiveGraph* getLiveGraph(const std::string& graph_name) const;
            LiveGraph*       getLiveGraph(const std::string& graph_name);

            /**
             * @brief shared_lock روی LiveGraph می‌گیرد (برای LockAlgorithm)
             * @return pair<LiveGraph*, shared_lock>  — گراف + lock
             *
             * lock تا وقتی که caller آن را آزاد نکند، write را block می‌کند.
             *
             * @example
             * ```cpp
             * auto [graph, lock] = manager.acquireReadLock("social_graph");
             * if (graph) {
             *     // در اینجا گراف تغییر نمی‌کند
             *     auto nbrs = graph->neighbors(uid, Direction::Out);
             * }
             * // با خروج از scope، lock آزاد می‌شود
             * ```
             */
            std::pair<const LiveGraph*, std::shared_lock<std::shared_mutex>>
            acquireReadLock(const std::string& graph_name) const;

            // ──────────────────────────────────────────────────────────
            // §5  StaticGraph snapshot (برای JobAlgorithm)
            // ──────────────────────────────────────────────────────────

            /**
             * @brief یک snapshot از LiveGraph می‌گیرد (برای JobAlgorithm)
             * @param graph_name  نام گراف
             * @param node_types  فیلتر node types (خالی = همه)
             * @param edge_types  فیلتر edge types (خالی = همه)
             * @return unique_ptr به StaticGraph — caller مالک است
             *
             * @note
             * - StaticGraph بعد از پایان job باید آزاد شود
             * - snapshot گرفتن کوتاه است (copy از adjacency lists)
             * - LiveGraph بعد از snapshot آزاد می‌شود تا تغییرات ادامه یابد
             *
             * @example
             * ```cpp
             * // تیم الگوریتم:
             * auto snapshot = manager.createSnapshot("social_graph");
             * auto result = pagerank_algo.run(*snapshot, {"20", "0.85"});
             * // snapshot آزاد می‌شود
             * ```
             */
            std::unique_ptr<StaticGraph> createSnapshot(
                    const std::string& graph_name,
                    const std::vector<std::string>& node_types = {},
                    const std::vector<std::string>& edge_types = {}) const;

            /**
             * @brief اجرای LockAlgorithm روی LiveGraph با shared_lock.
             *
             * الگوریتم در thread caller اجرا می‌شود و فقط باید از APIهای const
             * گراف استفاده کند.
             */
            algorithms::AlgoResult runLock(
                    const std::string& graph_name,
                    algorithms::LockAlgorithm& algo,
                    const std::vector<ExtId>& params) const;

            /**
             * @brief submit یک JobAlgorithm روی snapshot جدا از LiveGraph.
             *
             * snapshot در لحظه submit ساخته می‌شود، سپس الگوریتم در background
             * thread روی همان snapshot immutable اجرا می‌شود.
             *
             * @note شیء algo باید تا پایان job زنده بماند.
             */
            JobHandle submitJob(
                    const std::string& graph_name,
                    algorithms::JobAlgorithm& algo,
                    const std::vector<ExtId>& params) const;

            // ──────────────────────────────────────────────────────────
            // §6  Live Update hooks (DocEngine این‌ها را صدا می‌زند)
            // ──────────────────────────────────────────────────────────

            /**
             * @brief وقتی DocEngine::InsertOne فراخوانی می‌شود
             *
             * @details
             * GraphManager بررسی می‌کند:
             *  - کدام LIVE graph‌ها این collection را در node/edge mapping دارند؟
             *  - برای هر کدام: incrementally node یا edge اضافه می‌کند
             *  - هرگز کل گراف را rebuild نمی‌کند
             *
             * @param collection  نام collection که سند در آن درج شد
             * @param bson        محتوای سند جدید
             */
            void onDocumentInserted(const std::string& collection,
                                    const std::string& bson);

            /**
             * @brief وقتی DocEngine::UpdateById فراخوانی می‌شود
             *
             * @param collection   نام collection
             * @param old_bson     سند قبل از update
             * @param new_bson     سند بعد از update
             */
            void onDocumentUpdated(const std::string& collection,
                                   const std::string& old_bson,
                                   const std::string& new_bson);

            /**
             * @brief وقتی DocEngine::DeleteById فراخوانی می‌شود
             *
             * @param collection  نام collection
             * @param bson        محتوای سند حذف‌شده
             */
            void onDocumentDeleted(const std::string& collection,
                                   const std::string& bson);

            // ──────────────────────────────────────────────────────────
            // §7  آمار و وضعیت
            // ──────────────────────────────────────────────────────────

            /**
             * @brief آمار یک گراف
             */
            GraphStats getStats(const std::string& graph_name) const;

            /**
             * @brief آیا گراف build شده و در RAM است؟
             */
            bool isReady(const std::string& graph_name) const;

            /**
             * @brief وضعیت WAL یک گراف
             */
            struct WalStatus {
                uint64_t total_entries   = 0;
                uint64_t pending_entries = 0;
                bool     has_pending     = false;
            };
            WalStatus getWalStatus(const std::string& graph_name) const;

            /**
             * @brief آخرین رکوردهای WAL یک گراف را برمی‌گرداند.
             * @param limit حداکثر تعداد رکوردها؛ پیش‌فرض 20
             */
            std::vector<WalRecord> getRecentWalEntries(const std::string& graph_name,
                                                       size_t limit = 20) const;

            /**
             * @brief WAL قدیمی را پاک می‌کند (باید دوره‌ای صدا شود)
             *
             * NexoraQL: PURGE GRAPH WAL social_graph;
             */
            size_t purgeWAL(const std::string& graph_name);

        private:
            nexora::core::DocEngine* doc_engine_;   ///< source of truth
            std::filesystem::path    graph_dir_;    ///< پوشه فایل‌های گراف

            // ── Graph registry ──
            mutable std::mutex                                       registry_mutex_;
            std::unordered_map<std::string, std::unique_ptr<GraphHandle>> graphs_;

            // ── Internal helpers ──

            /**
             * @brief یک node را از یک document می‌سازد (برای buildGraph)
             */
            bool buildNodeFromDoc(LiveGraph& graph,
                                  const NodeMappingDef& mapping,
                                  const std::string& bson);

            /**
             * @brief edge(ها) را از یک document می‌سازد
             */
            bool buildEdgeFromDoc(LiveGraph& graph,
                                  const EdgeMappingDef& mapping,
                                  const std::string& bson);

            /**
             * @brief UNWIND: از یک array در document چند edge می‌سازد
             */
            bool buildEdgesUnwind(LiveGraph& graph,
                                  const EdgeMappingDef& mapping,
                                  const std::string& bson);

            /**
             * @brief یک فیلد را از BSON با dot-path استخراج می‌کند
             */
            static std::string extractField(const std::string& bson,
                                            const std::string& path);

            /**
             * @brief یک array field را از BSON استخراج می‌کند
             */
            static std::vector<std::string> extractArrayField(const std::string& bson,
                                                              const std::string& path);

            /**
             * @brief GraphDefinition را در RocksDB ذخیره می‌کند
             */
            bool persistDefinition(const GraphDefinition& def);

            /**
             * @brief GraphDefinition را از RocksDB می‌خواند
             */
            std::optional<GraphDefinition> loadDefinition(const std::string& name) const;

            /**
             * @brief همه تعریف‌ها را بارگذاری می‌کند
             */
            std::vector<GraphDefinition> loadAllDefinitions() const;

            /**
             * @brief یک GraphHandle جدید می‌سازد
             */
            std::unique_ptr<GraphHandle> makeHandle(const GraphDefinition& def);

            /**
             * @brief ارسال event به تمام LIVE graph‌هایی که به این collection علاقه دارند
             */
            void dispatchToLiveGraphs(const std::string& collection,
                                      const std::function<void(GraphHandle&,
                                                               const NodeMappingDef*,
                                                               const EdgeMappingDef*)>& fn);
        };

    } // namespace graph
} // namespace nexora


#endif //GITIGNORE_GRAPHMANAGER_H
