//
// Created by HOME on 6/19/2026.
//

/**
 * @file graph/GraphManager.cpp
 * @brief پیاده‌سازی GraphManager
 */

#include "GraphManager.h"
#include "../query/Evaluator.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace nexora {
    namespace graph {

// ══════════════════════════════════════════════════════════════
// §1  سازنده / مخرب
// ══════════════════════════════════════════════════════════════

        GraphManager::GraphManager(nexora::core::DocEngine* doc_engine,
                                   std::filesystem::path graph_dir)
                : doc_engine_(doc_engine), graph_dir_(std::move(graph_dir)) {
            std::filesystem::create_directories(graph_dir_);
        }

        GraphManager::~GraphManager() {
            shutdown();
        }

        void GraphManager::shutdown() {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            for (auto& [name, handle] : graphs_) {
                if (handle->wal)     handle->wal->close();
                if (handle->storage) handle->storage->close();
            }
        }

// ══════════════════════════════════════════════════════════════
// §2  ExtractField (ساده — از Evaluator استفاده می‌کند)
// ══════════════════════════════════════════════════════════════

        std::string GraphManager::extractField(const std::string& bson,
                                               const std::string& path) {
            thread_local nexora::query::Evaluator eval;
            auto fv = eval.ExtractField(bson, path);
            return fv.found ? fv.raw : "";
        }

        std::vector<std::string> GraphManager::extractArrayField(
                const std::string& bson, const std::string& path)
        {
            // پارس JSON array ساده
            thread_local nexora::query::Evaluator eval;
            auto fv = eval.ExtractField(bson, path);
            if (!fv.found || fv.raw.empty() || fv.raw[0] != '[') return {};

            std::vector<std::string> result;
            std::string inner = fv.raw.substr(1, fv.raw.size() > 2 ? fv.raw.size() - 2 : 0);
            std::istringstream ss(inner);
            std::string token;
            while (std::getline(ss, token, ',')) {
                // trim و حذف کوتیشن
                while (!token.empty() && (token.front() == ' ' || token.front() == '"'))
                    token.erase(token.begin());
                while (!token.empty() && (token.back() == ' ' || token.back() == '"'))
                    token.pop_back();
                if (!token.empty()) result.push_back(token);
            }
            return result;
        }

// ══════════════════════════════════════════════════════════════
// §3  Persistence تعریف گراف در RocksDB
// ══════════════════════════════════════════════════════════════

        namespace {
// کلیدهای مخفی در RocksDB برای تعریف گراف
            std::string defKey(const std::string& name) {
                return "graph:def:" + name;
            }
            std::string defListKey() {
                return "graph:deflist";
            }

// Serialization ساده برای GraphDefinition
            std::string serializeDef(const GraphDefinition& def) {
                std::ostringstream ss;
                ss << "name=" << def.name << "\n"
                   << "mode=" << (def.mode == GraphMode::Live ? "live" : "static") << "\n"
                   << "directed=" << (def.directed ? "1" : "0") << "\n"
                   << "heterogeneous=" << (def.heterogeneous ? "1" : "0") << "\n"
                   << "auto_build=" << (def.auto_build_on_startup ? "1" : "0") << "\n";

                for (const auto& nm : def.node_mappings) {
                    ss << "NODE:" << nm.node_type << "|" << nm.collection << "|"
                       << nm.key_path << "|";
                    for (size_t i = 0; i < nm.properties.size(); ++i) {
                        if (i) ss << ",";
                        ss << nm.properties[i];
                    }
                    ss << "|" << nm.filter_expr << "\n";
                }

                for (const auto& em : def.edge_mappings) {
                    ss << "EDGE:" << em.edge_type << "|" << em.collection << "|"
                       << em.source_path << "|" << em.source_node_type << "|"
                       << em.target_path << "|" << em.target_node_type << "|"
                       << (em.directed ? "1" : "0") << "|";
                    for (size_t i = 0; i < em.properties.size(); ++i) {
                        if (i) ss << ",";
                        ss << em.properties[i];
                    }
                    ss << "|";
                    if (em.unwind) {
                        ss << em.unwind->array_path << "," << em.unwind->alias;
                    }
                    ss << "\n";
                }
                return ss.str();
            }

            std::optional<GraphDefinition> deserializeDef(const std::string& data) {
                GraphDefinition def;
                std::istringstream ss(data);
                std::string line;
                while (std::getline(ss, line)) {
                    if (line.empty()) continue;
                    if (line.substr(0, 5) == "NODE:") {
                        std::istringstream ls(line.substr(5));
                        NodeMappingDef nm;
                        std::string tok;
                        std::getline(ls, nm.node_type,   '|');
                        std::getline(ls, nm.collection,  '|');
                        std::getline(ls, nm.key_path,    '|');
                        std::string props_str;
                        std::getline(ls, props_str, '|');
                        std::getline(ls, nm.filter_expr, '|');
                        if (!props_str.empty()) {
                            std::istringstream ps(props_str);
                            std::string p;
                            while (std::getline(ps, p, ','))
                                if (!p.empty()) nm.properties.push_back(p);
                        }
                        def.node_mappings.push_back(std::move(nm));
                    } else if (line.substr(0, 5) == "EDGE:") {
                        std::istringstream ls(line.substr(5));
                        EdgeMappingDef em;
                        std::string tok, dir_str, props_str, unwind_str;
                        std::getline(ls, em.edge_type,        '|');
                        std::getline(ls, em.collection,        '|');
                        std::getline(ls, em.source_path,       '|');
                        std::getline(ls, em.source_node_type,  '|');
                        std::getline(ls, em.target_path,       '|');
                        std::getline(ls, em.target_node_type,  '|');
                        std::getline(ls, dir_str,              '|');
                        std::getline(ls, props_str,            '|');
                        std::getline(ls, unwind_str,           '|');
                        em.directed = (dir_str == "1");
                        if (!props_str.empty()) {
                            std::istringstream ps(props_str);
                            std::string p;
                            while (std::getline(ps, p, ','))
                                if (!p.empty()) em.properties.push_back(p);
                        }
                        if (!unwind_str.empty()) {
                            auto comma = unwind_str.find(',');
                            if (comma != std::string::npos) {
                                em.unwind = UnwindConfig{
                                        unwind_str.substr(0, comma),
                                        unwind_str.substr(comma + 1)
                                };
                            }
                        }
                        def.edge_mappings.push_back(std::move(em));
                    } else {
                        auto eq = line.find('=');
                        if (eq == std::string::npos) continue;
                        std::string key = line.substr(0, eq);
                        std::string val = line.substr(eq + 1);
                        if      (key == "name")          def.name = val;
                        else if (key == "mode")          def.mode = (val == "live") ? GraphMode::Live : GraphMode::Static;
                        else if (key == "directed")      def.directed = (val == "1");
                        else if (key == "heterogeneous") def.heterogeneous = (val == "1");
                        else if (key == "auto_build")    def.auto_build_on_startup = (val == "1");
                    }
                }
                return def.name.empty() ? std::nullopt : std::optional<GraphDefinition>(def);
            }
        } // anonymous namespace

        bool GraphManager::persistDefinition(const GraphDefinition& def) {
            if (!doc_engine_) return false;
            // از RocksDB مستقیم DocEngine استفاده نمی‌کنیم — از یک key مخفی استفاده می‌کنیم
            // DocEngine::InsertOne را نمی‌توانیم استفاده کنیم چون این metadata است نه document
            // در عوض از GraphIdStore ذخیره می‌کنیم
            // (در این MVP از فایل ساده استفاده می‌کنیم)
            auto def_path = graph_dir_ / (def.name + ".graphdef");
            std::ofstream f(def_path);
            if (!f) return false;
            f << serializeDef(def);
            return true;
        }

        std::optional<GraphDefinition> GraphManager::loadDefinition(
                const std::string& name) const
        {
            auto def_path = graph_dir_ / (name + ".graphdef");
            if (!std::filesystem::exists(def_path)) return std::nullopt;
            std::ifstream f(def_path);
            if (!f) return std::nullopt;
            std::string data((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
            return deserializeDef(data);
        }

        std::vector<GraphDefinition> GraphManager::loadAllDefinitions() const {
            std::vector<GraphDefinition> result;
            for (const auto& entry : std::filesystem::directory_iterator(graph_dir_)) {
                if (entry.path().extension() != ".graphdef") continue;
                std::string name = entry.path().stem().string();
                auto def = loadDefinition(name);
                if (def) result.push_back(*def);
            }
            return result;
        }

// ══════════════════════════════════════════════════════════════
// §4  makeHandle
// ══════════════════════════════════════════════════════════════

        std::unique_ptr<GraphHandle> GraphManager::makeHandle(
                const GraphDefinition& def)
        {
            auto h = std::make_unique<GraphHandle>();
            h->definition = def;

            auto graph_subdir = graph_dir_ / def.name;
            std::filesystem::create_directories(graph_subdir);

            // Storage
            h->storage = std::make_unique<GraphStorage>(graph_subdir, def.name);
            if (!h->storage->open()) return nullptr;

            // WAL
            auto wal_path = graph_subdir / "graph.wal";
            h->wal = std::make_unique<GraphWAL>(wal_path, 86400);
            if (!h->wal->open()) return nullptr;

            // IdStore — از RocksDB DocEngine استفاده می‌کند
            rocksdb::DB* raw_db = nullptr;
            // DocEngine::getRocksDB() — این متد باید به DocEngine اضافه شود
            // در MVP از storage->metaPath() استفاده می‌کنیم
            // h->id_store = std::make_unique<GraphIdStore>(raw_db, def.name);

            // LiveGraph
            if (def.mode == GraphMode::Live) {
                h->live_graph = std::make_unique<LiveGraph>(
                        h->storage.get(), h->wal.get(), def.name);
            }

            return h;
        }

// ══════════════════════════════════════════════════════════════
// §5  Startup
// ══════════════════════════════════════════════════════════════

        bool GraphManager::startup() {
            auto defs = loadAllDefinitions();

            for (const auto& def : defs) {
                auto handle = makeHandle(def);
                if (!handle) continue;

                if (def.auto_build_on_startup && handle->live_graph) {
                    // بررسی meta
                    bool meta_ok = handle->storage->isMetaClean();

                    if (!meta_ok) {
                        // rebuild از DocEngine
                        std::lock_guard<std::mutex> lock(registry_mutex_);
                        graphs_[def.name] = std::move(handle);
                        buildGraph(def.name);
                    } else {
                        // بارگذاری از فایل
                        handle->live_graph->loadFromDisk();
                        handle->live_graph->replayWAL();

                        std::lock_guard<std::mutex> lock(registry_mutex_);
                        handle->built = true;
                        graphs_[def.name] = std::move(handle);
                    }
                } else {
                    std::lock_guard<std::mutex> lock(registry_mutex_);
                    graphs_[def.name] = std::move(handle);
                }
            }
            return true;
        }

// ══════════════════════════════════════════════════════════════
// §6  createGraph / addMapping / dropGraph
// ══════════════════════════════════════════════════════════════

        bool GraphManager::createGraph(const GraphDefinition& def) {
            if (def.name.empty()) return false;

            std::lock_guard<std::mutex> lock(registry_mutex_);
            if (graphs_.count(def.name)) return false;  // از قبل وجود دارد

            auto handle = makeHandle(def);
            if (!handle) return false;

            if (!persistDefinition(def)) return false;

            graphs_[def.name] = std::move(handle);
            return true;
        }

        bool GraphManager::addNodeMapping(const std::string& graph_name,
                                          const NodeMappingDef& mapping) {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            auto it = graphs_.find(graph_name);
            if (it == graphs_.end()) return false;

            it->second->definition.node_mappings.push_back(mapping);
            return persistDefinition(it->second->definition);
        }

        bool GraphManager::addEdgeMapping(const std::string& graph_name,
                                          const EdgeMappingDef& mapping) {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            auto it = graphs_.find(graph_name);
            if (it == graphs_.end()) return false;

            it->second->definition.edge_mappings.push_back(mapping);
            return persistDefinition(it->second->definition);
        }

        bool GraphManager::dropGraph(const std::string& graph_name) {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            auto it = graphs_.find(graph_name);
            if (it == graphs_.end()) return false;

            if (it->second->wal)     it->second->wal->close();
            if (it->second->storage) it->second->storage->close();

            // حذف فایل‌ها
            auto graph_subdir = graph_dir_ / graph_name;
            std::filesystem::remove_all(graph_subdir);
            std::filesystem::remove(graph_dir_ / (graph_name + ".graphdef"));

            graphs_.erase(it);
            return true;
        }

        std::optional<GraphDefinition> GraphManager::getDefinition(
                const std::string& graph_name) const
        {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            auto it = graphs_.find(graph_name);
            if (it == graphs_.end()) return std::nullopt;
            return it->second->definition;
        }

        std::vector<std::string> GraphManager::listGraphs() const {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            std::vector<std::string> result;
            for (const auto& [name, _] : graphs_) result.push_back(name);
            return result;
        }

// ══════════════════════════════════════════════════════════════
// §7  buildGraph — قلب ارتباط DocEngine ↔ LiveGraph
// ══════════════════════════════════════════════════════════════

        bool GraphManager::buildNodeFromDoc(LiveGraph& graph,
                                            const NodeMappingDef& mapping,
                                            const std::string& bson) {
            // فیلتر (اگر filter_expr تنظیم شده)
            if (!mapping.filter_expr.empty()) {
                // ساده‌ترین فیلتر: "field=value"
                // در MVP این را skip می‌کنیم — تیم Parser این را گسترش می‌دهد
            }

            std::string ext_id = extractField(bson, mapping.key_path);
            if (ext_id.empty()) return false;

            graph.addNode(ext_id, mapping.node_type, false);
            return true;
        }

        bool GraphManager::buildEdgeFromDoc(LiveGraph& graph,
                                            const EdgeMappingDef& mapping,
                                            const std::string& bson) {
            if (mapping.unwind.has_value()) {
                return buildEdgesUnwind(graph, mapping, bson);
            }

            // روش ۱ یا ۲: یک src و یک dst
            std::string src_id = extractField(bson, mapping.source_path);
            std::string dst_id = extractField(bson, mapping.target_path);

            if (src_id.empty() || dst_id.empty()) return false;

            graph.addEdge(src_id, dst_id, mapping.edge_type, mapping.directed);
            return true;
        }

        bool GraphManager::buildEdgesUnwind(LiveGraph& graph,
                                            const EdgeMappingDef& mapping,
                                            const std::string& bson) {
            // UNWIND: از array، چند edge می‌سازیم
            const auto& uw = *mapping.unwind;

            auto items = extractArrayField(bson, uw.array_path);
            if (items.empty()) {
                // array وجود ندارد یا خالی است — نه خطا، فقط هیچ edge‌ای نساختیم
                return false;
            }

            // مقادیر ثابتی که نیاز داریم (یک بار محاسبه کن)
            // source_path یا target_path ممکن است alias باشد
            const bool source_is_alias = (mapping.source_path == uw.alias);
            const bool target_is_alias = (mapping.target_path == uw.alias);

            // اگر نه source و نه target به alias اشاره نمی‌کنند، mapping اشتباه است
            if (!source_is_alias && !target_is_alias) return false;

            // مقدار ثابت (طرف غیر-alias)
            const std::string fixed_src = source_is_alias ? "" : extractField(bson, mapping.source_path);
            const std::string fixed_dst = target_is_alias ? "" : extractField(bson, mapping.target_path);

            // اگر طرف ثابت خالی است، mapping اشتباه یا فیلد وجود ندارد
            if (!source_is_alias && fixed_src.empty()) return false;
            if (!target_is_alias && fixed_dst.empty()) return false;

            int built = 0;
            for (const auto& item : items) {
                if (item.empty()) continue;

                const std::string& src_id = source_is_alias ? item : fixed_src;
                const std::string& dst_id = target_is_alias ? item : fixed_dst;

                if (src_id.empty() || dst_id.empty()) continue;

                auto eid = graph.addEdge(src_id, dst_id, mapping.edge_type, mapping.directed);
                if (eid != kInvalidEdgeId) ++built;
            }

            // true اگر حداقل یک edge ساخته شد
            return built > 0;
        }

        BuildResult GraphManager::buildGraph(const std::string& graph_name) {
            BuildResult result;
            if (!doc_engine_) {
                result.error_msg = "DocEngine is null";
                return result;
            }

            auto t0 = std::chrono::steady_clock::now();

            GraphHandle* handle = nullptr;
            {
                std::lock_guard<std::mutex> lock(registry_mutex_);
                auto it = graphs_.find(graph_name);
                if (it == graphs_.end()) {
                    result.error_msg = "Graph '" + graph_name + "' not found";
                    return result;
                }
                handle = it->second.get();
            }

            if (!handle->live_graph) {
                result.error_msg = "Graph is STATIC — use snapshot";
                return result;
            }

            // clear + set dirty
            handle->storage->updateMetaState(GraphState::Building);
            handle->live_graph->clear();

            const GraphDefinition& def = handle->definition;
            LiveGraph& graph = *handle->live_graph;

            // ── مرحله ۱: ساخت nodes از collections ──
            for (const auto& nm : def.node_mappings) {
                doc_engine_->IterateCollection(
                        nm.collection,
                        [&](const std::string& doc_id, const std::string& bson) -> bool {
                            if (buildNodeFromDoc(graph, nm, bson))
                                ++result.nodes_built;
                            return true;
                        }
                );
            }

            // ── مرحله ۲: ساخت edges از collections ──
            for (const auto& em : def.edge_mappings) {
                doc_engine_->IterateCollection(
                        em.collection,
                        [&](const std::string& doc_id, const std::string& bson) -> bool {
                            if (buildEdgeFromDoc(graph, em, bson))
                                ++result.edges_built;
                            return true;
                        }
                );
            }

            // ── مرحله ۳: به‌روزرسانی meta ──
            GraphMeta meta{};
            meta.magic        = 0x4E455847u;
            meta.version      = 1;
            meta.kind         = (def.mode == GraphMode::Live) ? GraphKind::Live : GraphKind::Static;
            meta.state        = GraphState::Clean;
            meta.active_nodes = result.nodes_built;
            meta.active_edges = result.edges_built;
            meta.updated_at   = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            std::strncpy(meta.graph_name, graph_name.c_str(),
                         sizeof(meta.graph_name) - 1);
            handle->storage->writeMeta(meta);

            handle->built = true;

            auto t1 = std::chrono::steady_clock::now();
            result.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            result.success = true;
            return result;
        }

        bool GraphManager::renderGraph(const std::string& graph_name) {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            auto it = graphs_.find(graph_name);
            if (it == graphs_.end() || !it->second->live_graph) return false;

            it->second->live_graph->loadFromDisk();
            it->second->live_graph->replayWAL();
            it->second->built = true;
            return true;
        }

        BuildResult GraphManager::refreshGraph(const std::string& graph_name) {
            {
                std::lock_guard<std::mutex> lock(registry_mutex_);
                auto it = graphs_.find(graph_name);
                if (it != graphs_.end() && it->second->live_graph)
                    it->second->live_graph->clear();
            }
            return buildGraph(graph_name);
        }

        bool GraphManager::compactGraph(const std::string& graph_name) {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            auto it = graphs_.find(graph_name);
            if (it == graphs_.end() || !it->second->live_graph) return false;
            return it->second->live_graph->compact();
        }

// ══════════════════════════════════════════════════════════════
// §8  دسترسی به LiveGraph
// ══════════════════════════════════════════════════════════════

        const LiveGraph* GraphManager::getLiveGraph(const std::string& name) const {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            auto it = graphs_.find(name);
            if (it == graphs_.end() || !it->second->live_graph) return nullptr;
            return it->second->live_graph.get();
        }

        LiveGraph* GraphManager::getLiveGraph(const std::string& name) {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            auto it = graphs_.find(name);
            if (it == graphs_.end() || !it->second->live_graph) return nullptr;
            return it->second->live_graph.get();
        }

        std::pair<const LiveGraph*, std::shared_lock<std::shared_mutex>>
        GraphManager::acquireReadLock(const std::string& graph_name) const {
            std::lock_guard<std::mutex> reg_lock(registry_mutex_);
            auto it = graphs_.find(graph_name);
            if (it == graphs_.end() || !it->second->live_graph)
                return {nullptr, std::shared_lock<std::shared_mutex>{}};

            std::shared_lock<std::shared_mutex> sh(it->second->rw_mutex);
            return {it->second->live_graph.get(), std::move(sh)};
        }

// ══════════════════════════════════════════════════════════════
// §9  StaticGraph snapshot
// ══════════════════════════════════════════════════════════════

        std::unique_ptr<StaticGraph> GraphManager::createSnapshot(
                const std::string& graph_name,
                const std::vector<std::string>& node_types,
                const std::vector<std::string>& edge_types) const
        {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            auto it = graphs_.find(graph_name);
            if (it == graphs_.end() || !it->second->live_graph) return nullptr;

            // snapshot با shared_lock (سریع — فقط copy ساختار)
            std::shared_lock<std::shared_mutex> sh(it->second->rw_mutex);
            return std::make_unique<StaticGraph>(
                    *it->second->live_graph, node_types, edge_types, graph_name);
        }

// ══════════════════════════════════════════════════════════════
// §10  Live Update hooks
// ══════════════════════════════════════════════════════════════

        void GraphManager::dispatchToLiveGraphs(
                const std::string& collection,
                const std::function<void(GraphHandle&,
                                         const NodeMappingDef*,
                                         const EdgeMappingDef*)>& fn)
        {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            for (auto& [name, handle] : graphs_) {
                if (handle->definition.mode != GraphMode::Live) continue;
                if (!handle->live_graph || !handle->built) continue;

                // بررسی node mappings
                for (const auto& nm : handle->definition.node_mappings) {
                    if (nm.collection == collection)
                        fn(*handle, &nm, nullptr);
                }
                // بررسی edge mappings
                for (const auto& em : handle->definition.edge_mappings) {
                    if (em.collection == collection)
                        fn(*handle, nullptr, &em);
                }
            }
        }

        void GraphManager::onDocumentInserted(const std::string& collection,
                                              const std::string& bson) {
            dispatchToLiveGraphs(collection,
                                 [&](GraphHandle& h, const NodeMappingDef* nm, const EdgeMappingDef* em) {
                                     std::unique_lock<std::shared_mutex> lock(h.rw_mutex);
                                     if (nm) buildNodeFromDoc(*h.live_graph, *nm, bson);
                                     if (em) buildEdgeFromDoc(*h.live_graph, *em, bson);
                                 });
        }

        void GraphManager::onDocumentUpdated(const std::string& collection,
                                             const std::string& old_bson,
                                             const std::string& new_bson) {
            dispatchToLiveGraphs(collection,
                                 [&](GraphHandle& h, const NodeMappingDef* nm, const EdgeMappingDef* em) {
                                     std::unique_lock<std::shared_mutex> lock(h.rw_mutex);
                                     // حذف قدیمی، اضافه جدید
                                     if (nm) {
                                         std::string old_id = extractField(old_bson, nm->key_path);
                                         if (!old_id.empty()) h.live_graph->removeNode(old_id);
                                         buildNodeFromDoc(*h.live_graph, *nm, new_bson);
                                     }
                                     if (em) {
                                         // حذف edge قدیمی
                                         if (!em->unwind) {
                                             std::string old_src = extractField(old_bson, em->source_path);
                                             std::string old_dst = extractField(old_bson, em->target_path);
                                             if (!old_src.empty() && !old_dst.empty())
                                                 h.live_graph->removeEdge(old_src, old_dst, em->edge_type);
                                         }
                                         buildEdgeFromDoc(*h.live_graph, *em, new_bson);
                                     }
                                 });
        }

        void GraphManager::onDocumentDeleted(const std::string& collection,
                                             const std::string& bson) {
            dispatchToLiveGraphs(collection,
                                 [&](GraphHandle& h, const NodeMappingDef* nm, const EdgeMappingDef* em) {
                                     std::unique_lock<std::shared_mutex> lock(h.rw_mutex);
                                     if (nm) {
                                         std::string ext_id = extractField(bson, nm->key_path);
                                         if (!ext_id.empty()) h.live_graph->removeNode(ext_id);
                                     }
                                     if (em && !em->unwind) {
                                         std::string src = extractField(bson, em->source_path);
                                         std::string dst = extractField(bson, em->target_path);
                                         if (!src.empty() && !dst.empty())
                                             h.live_graph->removeEdge(src, dst, em->edge_type);
                                     }
                                 });
        }

// ══════════════════════════════════════════════════════════════
// §11  Stats و WAL
// ══════════════════════════════════════════════════════════════

        GraphStats GraphManager::getStats(const std::string& graph_name) const {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            auto it = graphs_.find(graph_name);
            if (it == graphs_.end() || !it->second->live_graph) return {};
            return it->second->live_graph->stats();
        }

        bool GraphManager::isReady(const std::string& graph_name) const {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            auto it = graphs_.find(graph_name);
            return it != graphs_.end() && it->second->built;
        }

        GraphManager::WalStatus GraphManager::getWalStatus(
                const std::string& graph_name) const
        {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            auto it = graphs_.find(graph_name);
            if (it == graphs_.end() || !it->second->wal) return {};
            WalStatus ws;
            ws.total_entries   = it->second->wal->totalEntries();
            ws.has_pending     = it->second->wal->hasPendingEntries();
            ws.pending_entries = it->second->wal->loadUnapplied().size();
            return ws;
        }

        size_t GraphManager::purgeWAL(const std::string& graph_name) {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            auto it = graphs_.find(graph_name);
            if (it == graphs_.end() || !it->second->wal) return 0;
            return it->second->wal->purgeOld();
        }

    } // namespace graph
} // namespace nexora