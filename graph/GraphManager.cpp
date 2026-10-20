//
// Created by HOME on 6/19/2026.
//

/**
 * @file graph/GraphManager.cpp
 * @brief پیاده‌سازی GraphManager
 */

#include "GraphManager.h"
#include "DurableFile.h"
#include "BulkGraphBuilder.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace nexora {
    namespace graph {

// ══════════════════════════════════════════════════════════════
// §1  سازنده / مخرب
// ══════════════════════════════════════════════════════════════

        GraphManager::GraphManager(nexora::core::DocEngine* doc_engine,
                                   std::filesystem::path graph_dir, WalOptions wal_options,SchedulerOptions scheduler_options)
                : doc_engine_(doc_engine), graph_dir_(std::move(graph_dir)), wal_options_(wal_options),
                  scheduler_(std::make_unique<JobScheduler>(scheduler_options)) {
            std::filesystem::create_directories(graph_dir_);
            directory_lease_=std::make_unique<detail::DirectoryLease>(graph_dir_);
        }

        GraphManager::~GraphManager() {
            shutdown();
        }

        void GraphManager::shutdown() {
            scheduler_->shutdown(); // No registry lock while workers finish.
            std::lock_guard<std::mutex> lock(registry_mutex_);
            for (auto& [name, handle] : graphs_) {
                if (handle->wal)     handle->wal->close();
                if (handle->storage) handle->storage->close();
            }
        }
        bool GraphManager::prepareShutdown() {
            scheduler_->shutdown();
            std::vector<std::shared_ptr<GraphHandle>> handles;
            {std::lock_guard lock(registry_mutex_); for(const auto& item:graphs_) handles.push_back(item.second);}
            for(const auto& handle:handles) {
                std::unique_lock lock(handle->rw_mutex);
                if(handle->live_graph && !handle->live_graph->checkpointWAL()) return false;
                if(handle->storage && !handle->storage->durableSync()) return false;
            }
            return true;
        }

// ══════════════════════════════════════════════════════════════
// §2  ExtractField (ساده — از Evaluator استفاده می‌کند)
// ══════════════════════════════════════════════════════════════

        std::string GraphManager::extractField(const std::string& bson,
                                               const std::string& path) {
            const auto document = nlohmann::json::parse(bson, nullptr, false);
            if (document.is_discarded()) return {};
            const nlohmann::json* value = &document;
            std::size_t start = 0;
            while (start <= path.size()) {
                const std::size_t separator = path.find('.', start);
                const std::string component = path.substr(
                        start, separator == std::string::npos
                                ? std::string::npos : separator - start);
                if (!value->is_object()) return {};
                const auto found = value->find(component);
                if (found == value->end()) return {};
                value = &*found;
                if (separator == std::string::npos) break;
                start = separator + 1;
            }
            if (value->is_string()) return value->get<std::string>();
            if (value->is_boolean()) return value->get<bool>() ? "true" : "false";
            if (value->is_number()) return value->dump();
            return {};
        }

        std::vector<std::string> GraphManager::extractArrayField(
                const std::string& bson, const std::string& path)
        {
            const auto document = nlohmann::json::parse(bson, nullptr, false);
            if (document.is_discarded()) return {};
            const nlohmann::json* value = &document;
            std::size_t start = 0;
            while (start <= path.size()) {
                const std::size_t separator = path.find('.', start);
                const std::string component = path.substr(
                        start, separator == std::string::npos
                                ? std::string::npos : separator - start);
                if (!value->is_object()) return {};
                const auto found = value->find(component);
                if (found == value->end()) return {};
                value = &*found;
                if (separator == std::string::npos) break;
                start = separator + 1;
            }
            if (!value->is_array()) return {};
            std::vector<std::string> result;
            result.reserve(value->size());
            for (const auto& item : *value) {
                if (item.is_string()) result.push_back(item.get<std::string>());
                else if (item.is_boolean())
                    result.emplace_back(item.get<bool>() ? "true" : "false");
                else if (item.is_number()) result.push_back(item.dump());
            }
            return result;
        }

// ══════════════════════════════════════════════════════════════
// §3  Persistence تعریف گراف در RocksDB
// ══════════════════════════════════════════════════════════════

        namespace {
// کلیدهای مخفی در RocksDB برای تعریف گراف
//            std::string defKey(const std::string& name) {
//                return "graph:def:" + name;
//            }
//            std::string defListKey() {
//                return "graph:deflist";
//            }

// Serialization ساده برای GraphDefinition
            std::string serializeDef(const GraphDefinition& def) {
                nlohmann::json document = {
                    {"format_version", 2}, {"name", def.name},
                    {"mode", def.mode == GraphMode::Live ? "live" : "static"},
                    {"directed", def.directed}, {"heterogeneous", def.heterogeneous},
                    {"auto_build", def.auto_build_on_startup},
                    {"nodes", nlohmann::json::array()}, {"edges", nlohmann::json::array()}};
                for (const auto& nm : def.node_mappings) {
                    document["nodes"].push_back({{"type", nm.node_type}, {"collection", nm.collection},
                        {"key", nm.key_path}, {"properties", nm.properties}, {"filter", nm.filter_expr}});
                }
                for (const auto& em : def.edge_mappings) {
                    nlohmann::json edge = {{"type", em.edge_type}, {"collection", em.collection},
                        {"source", em.source_path}, {"source_type", em.source_node_type},
                        {"target", em.target_path}, {"target_type", em.target_node_type},
                        {"directed", em.directed}, {"properties", em.properties}};
                    if (em.unwind)
                        edge["unwind"] = {{"path", em.unwind->array_path}, {"alias", em.unwind->alias}};
                    document["edges"].push_back(std::move(edge));
                }
                return document.dump();
            }

            std::optional<GraphDefinition> deserializeDef(const std::string& data) {
                GraphDefinition def;
                const auto first = data.find_first_not_of(" \t\r\n");
                if (first != std::string::npos && data[first] == '{') {
                    try {
                        const auto document = nlohmann::json::parse(data);
                        if (document.at("format_version") != 2) return std::nullopt;
                        def.name = document.at("name").get<std::string>();
                        const auto mode = document.at("mode").get<std::string>();
                        if (mode != "live" && mode != "static") return std::nullopt;
                        def.mode = mode == "live" ? GraphMode::Live : GraphMode::Static;
                        def.directed = document.at("directed").get<bool>();
                        def.heterogeneous = document.at("heterogeneous").get<bool>();
                        def.auto_build_on_startup = document.at("auto_build").get<bool>();
                        if (!document.at("nodes").is_array() || !document.at("edges").is_array())
                            return std::nullopt;
                        for (const auto& node : document.at("nodes")) {
                            NodeMappingDef nm;
                            nm.node_type = node.at("type").get<std::string>();
                            nm.collection = node.at("collection").get<std::string>();
                            nm.key_path = node.at("key").get<std::string>();
                            nm.properties = node.at("properties").get<std::vector<std::string>>();
                            nm.filter_expr = node.at("filter").get<std::string>();
                            def.node_mappings.push_back(std::move(nm));
                        }
                        for (const auto& edge : document.at("edges")) {
                            EdgeMappingDef em;
                            em.edge_type = edge.at("type").get<std::string>();
                            em.collection = edge.at("collection").get<std::string>();
                            em.source_path = edge.at("source").get<std::string>();
                            em.source_node_type = edge.at("source_type").get<std::string>();
                            em.target_path = edge.at("target").get<std::string>();
                            em.target_node_type = edge.at("target_type").get<std::string>();
                            em.directed = edge.at("directed").get<bool>();
                            em.properties = edge.at("properties").get<std::vector<std::string>>();
                            if (edge.contains("unwind"))
                                em.unwind = UnwindConfig{edge.at("unwind").at("path").get<std::string>(),
                                                         edge.at("unwind").at("alias").get<std::string>()};
                            def.edge_mappings.push_back(std::move(em));
                        }
                        return def;
                    } catch (const nlohmann::json::exception&) { return std::nullopt; }
                }
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

            std::future<algorithms::AlgoResult> readyAlgoFuture(
                    algorithms::AlgoResult result) {
                std::promise<algorithms::AlgoResult> promise;
                promise.set_value(std::move(result));
                return promise.get_future();
            }
        } // anonymous namespace

        bool GraphManager::persistDefinition(const GraphDefinition& def) {
            if (!doc_engine_ || !detail::ValidGraphName(def.name)) return false;
            // از RocksDB مستقیم DocEngine استفاده نمی‌کنیم — از یک key مخفی استفاده می‌کنیم
            // DocEngine::InsertOne را نمی‌توانیم استفاده کنیم چون این metadata است نه document
            // در عوض از GraphIdStore ذخیره می‌کنیم
            // (در این MVP از فایل ساده استفاده می‌کنیم)
            auto def_path = graph_dir_ / (def.name + ".graphdef");
            return detail::AtomicWriteFile(def_path, serializeDef(def));
        }

        std::optional<GraphDefinition> GraphManager::loadDefinition(
                const std::string& name) const
        {
            if (!detail::ValidGraphName(name)) return std::nullopt;
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
                if (!def || def->name != name)
                    throw std::runtime_error("Invalid graph definition: " + entry.path().string());
                result.push_back(*def);
            }
            return result;
        }

// ══════════════════════════════════════════════════════════════
// §4  makeHandle
// ══════════════════════════════════════════════════════════════

        std::shared_ptr<GraphHandle> GraphManager::makeHandle(
                const GraphDefinition& def)
        {
            if (!detail::ValidGraphName(def.name)) return nullptr;
            auto h = std::make_shared<GraphHandle>();
            h->definition = def;

            auto graph_subdir = graph_dir_ / def.name;
            std::filesystem::create_directories(graph_subdir);

            // Storage
            h->storage = std::make_unique<GraphStorage>(graph_subdir, def.name);
            if (!h->storage->open())
                throw std::runtime_error("Graph storage startup failed: "+h->storage->lastError());

            // WAL
            auto wal_path = graph_subdir / "graph.wal";
            h->wal = std::make_unique<GraphWAL>(wal_path, 86400, wal_options_);
            if (!h->wal->open())
                throw std::runtime_error("Graph WAL startup failed: " + h->wal->lastError());

            // Physical identities and type dictionaries live in the same
            // GraphStorage generation as the records, including compaction.

            // Backing graph used for both modes.
            // Live graphs receive incremental updates; Static graphs are only
            // rebuilt/rendered explicitly and then queried read-only.
            h->live_graph = std::make_unique<LiveGraph>(
                    h->storage.get(), h->wal.get(), def.name);

            return h;
        }

// ══════════════════════════════════════════════════════════════
// §5  Startup
// ══════════════════════════════════════════════════════════════

        bool GraphManager::recoverStoredGraphs() {
            { std::lock_guard lock(registry_mutex_); if(!graphs_.empty()) return false; }
            std::unordered_map<std::string,std::shared_ptr<GraphHandle>> recovered;
            for(const auto& entry:std::filesystem::directory_iterator(graph_dir_)) {
                if(entry.path().extension()!=".graphdef") continue;
                const auto name=entry.path().stem().string();
                const auto definition=loadDefinition(name);
                if(!definition || definition->name!=name) throw std::runtime_error("corrupt graph definition: "+name);
                if(!std::filesystem::exists(graph_dir_/name/(name+".manifest")))
                    throw std::runtime_error("missing graph storage manifest: "+name);
                auto handle=makeHandle(*definition);
                const auto meta=handle ? handle->storage->readMeta() : std::nullopt;
                if(!meta || meta->state==GraphState::Building || meta->state==GraphState::Corrupted)
                    throw std::runtime_error("incomplete/corrupt graph needs explicit source rebuild: "+name);
                if(!handle || !handle->live_graph->loadFromDisk()) return false;
                handle->live_graph->replayWAL();
                if(handle->wal->hasPendingEntries()) throw std::runtime_error("graph WAL recovery incomplete: "+name);
                handle->built=true; recovered.emplace(name,std::move(handle));
            }
            for(const auto& entry:std::filesystem::directory_iterator(graph_dir_))
                if(entry.is_directory() && !recovered.count(entry.path().filename().string()))
                    throw std::runtime_error("orphan graph directory without definition: "+entry.path().string());
            std::lock_guard lock(registry_mutex_);
            if(!graphs_.empty()) return false;
            graphs_=std::move(recovered); return true;
        }

        bool GraphManager::startup() {
            auto defs = loadAllDefinitions();

            for (const auto& def : defs) {
                auto handle = makeHandle(def);
                if (!handle) return false;

                if (def.auto_build_on_startup && handle->live_graph) {
                    // بررسی meta
                    bool meta_ok = handle->storage->isMetaClean();

                    if (!meta_ok) {
                        // rebuild از DocEngine
                        {
                            std::lock_guard<std::mutex> lock(registry_mutex_);
                            graphs_[def.name] = std::move(handle);
                        }
                        // buildGraph acquires registry_mutex_ itself.
                        if (!buildGraph(def.name).success) return false;
                    } else {
                        // بارگذاری از فایل
                        if (!handle->live_graph->loadFromDisk()) return false;
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
            if (!detail::ValidGraphName(def.name)) return false;

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
            auto owner=findHandle(graph_name);
            if (!owner) return false;
            std::unique_lock lock(owner->rw_mutex);

            auto updated = owner->definition;
            updated.node_mappings.push_back(mapping);
            if (!persistDefinition(updated)) return false;
            owner->definition = std::move(updated);
            owner->built = false;
            return true;
        }

        bool GraphManager::addEdgeMapping(const std::string& graph_name,
                                          const EdgeMappingDef& mapping) {
            auto owner=findHandle(graph_name);
            if (!owner) return false;
            std::unique_lock lock(owner->rw_mutex);

            auto updated = owner->definition;
            updated.edge_mappings.push_back(mapping);
            if (!persistDefinition(updated)) return false;
            owner->definition = std::move(updated);
            owner->built = false;
            return true;
        }

        bool GraphManager::dropGraph(const std::string& graph_name) {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            auto it = graphs_.find(graph_name);
            if (it == graphs_.end()) return false;
            // Never close backing files while an operation or lease owns them.
            // Busy is retryable; waiting here would recreate registry/graph ABBA.
            if (it->second.use_count()!=1) return false;

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
            auto owner=findHandle(graph_name);
            if (!owner) return std::nullopt;
            std::shared_lock lock(owner->rw_mutex);
            return owner->definition;
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
                                            const std::string& bson,BulkGraphBuilder* bulk) {
            // فیلتر (اگر filter_expr تنظیم شده)
            if (!mapping.filter_expr.empty()) {
                // ساده‌ترین فیلتر: "field=value"
                // در MVP این را skip می‌کنیم — تیم Parser این را گسترش می‌دهد
            }

            std::string ext_id = extractField(bson, mapping.key_path);
            if (ext_id.empty()) return false;

            if (bulk) bulk->addNode(ext_id,mapping.node_type);
            else graph.addNode(ext_id, mapping.node_type, false);
            return true;
        }

        bool GraphManager::buildEdgeFromDoc(LiveGraph& graph,
                                            const EdgeMappingDef& mapping,
                                            const std::string& bson,BulkGraphBuilder* bulk) {
            if (mapping.unwind.has_value()) {
                return buildEdgesUnwind(graph, mapping, bson,bulk);
            }

            // روش ۱ یا ۲: یک src و یک dst
            std::string src_id = extractField(bson, mapping.source_path);
            std::string dst_id = extractField(bson, mapping.target_path);

            if (src_id.empty() || dst_id.empty()) return false;

            if (bulk) bulk->addEdge(src_id,dst_id,mapping.edge_type,mapping.directed);
            else graph.addEdge(src_id, dst_id, mapping.edge_type, mapping.directed);
            return true;
        }

        bool GraphManager::buildEdgesUnwind(LiveGraph& graph,
                                            const EdgeMappingDef& mapping,
                                            const std::string& bson,BulkGraphBuilder* bulk) {
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

                if (bulk) { bulk->addEdge(src_id,dst_id,mapping.edge_type,mapping.directed); ++built; }
                else if (graph.addEdge(src_id,dst_id,mapping.edge_type,mapping.directed)!=kInvalidEdgeId) ++built;
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

            std::shared_ptr<GraphHandle> handle;
            {
                std::lock_guard<std::mutex> lock(registry_mutex_);
                auto it = graphs_.find(graph_name);
                if (it == graphs_.end()) {
                    result.error_msg = "Graph '" + graph_name + "' not found";
                    return result;
                }
                handle = it->second;
            }

            std::unique_lock operation_lock(handle->rw_mutex);

            if (!handle->live_graph)
                handle->live_graph = std::make_unique<LiveGraph>(
                        handle->storage.get(), handle->wal.get(), graph_name);

            // clear + set dirty
            handle->built = false;
            if (!handle->storage->updateMetaState(GraphState::Building)) {
                result.error_msg = "Unable to persist graph Building state";
                return result;
            }
            handle->live_graph->clear();

            const GraphDefinition& def = handle->definition;
            LiveGraph& graph = *handle->live_graph;

            // ── مرحله ۱: ساخت nodes از collections ──
            try {
            BulkGraphBuilder bulk(graph);
            for (const auto& nm : def.node_mappings) {
                doc_engine_->IterateCollection(
                        nm.collection,
                        [&](const std::string& doc_id, const std::string& bson) -> bool {
                            if (buildNodeFromDoc(graph, nm, bson,&bulk))
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
                            if (buildEdgeFromDoc(graph, em, bson,&bulk))
                                ++result.edges_built;
                            return true;
                        }
                );
            }
            bulk.flush();
            result.nodes_built=graph.activeNodeCount();
            result.edges_built=graph.activeEdgeCount();
            } catch (const std::exception& error) {
                result.error_msg = std::string("Graph source scan failed: ") + error.what();
                return result;
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
            if (!handle->storage->writeMeta(meta)) {
                result.error_msg = "Unable to persist graph metadata";
                return result;
            }

            handle->built = true;

            auto t1 = std::chrono::steady_clock::now();
            result.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            result.success = true;
            return result;
        }

        bool GraphManager::renderGraph(const std::string& graph_name) {
            auto owner=findHandle(graph_name);
            if (!owner || !owner->live_graph) return false;
            std::unique_lock lock(owner->rw_mutex);

            owner->built = false;
            if (!owner->live_graph->loadFromDisk()) return false;
            owner->live_graph->replayWAL();
            owner->built = true;
            return true;
        }

        bool GraphManager::checkpointGraph(const std::string& graph_name) {
            auto owner=findHandle(graph_name);
            return owner && owner->live_graph && owner->live_graph->checkpointWAL();
        }

        BuildResult GraphManager::refreshGraph(const std::string& graph_name) {
            return buildGraph(graph_name);
        }

        bool GraphManager::compactGraph(const std::string& graph_name) {
            auto owner=findHandle(graph_name);
            if (!owner || !owner->live_graph) return false;
            std::unique_lock lock(owner->rw_mutex);
            return owner->live_graph->compact();
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

        std::shared_ptr<GraphHandle> GraphManager::findHandle(const std::string& name) const {
            std::lock_guard lock(registry_mutex_);
            const auto found=graphs_.find(name);
            return found==graphs_.end() ? nullptr : found->second;
        }

        std::shared_ptr<LiveGraph> GraphManager::shareLiveGraph(const std::string& name) const {
            auto owner=findHandle(name);
            if (!owner || !owner->live_graph) return {};
            auto* graph=owner->live_graph.get();
            return std::shared_ptr<LiveGraph>(std::move(owner),graph);
        }

        std::pair<const LiveGraph*, GraphReadLease>
        GraphManager::acquireReadLock(const std::string& graph_name) const {
            auto owner=findHandle(graph_name);
            if (!owner || !owner->live_graph) return {nullptr,GraphReadLease{}};
            const auto* graph=owner->live_graph.get();
            return {graph,GraphReadLease(std::move(owner))};
        }

// ══════════════════════════════════════════════════════════════
// §9  StaticGraph snapshot
// ══════════════════════════════════════════════════════════════

        std::unique_ptr<StaticGraph> GraphManager::createSnapshot(
                const std::string& graph_name,
                const std::vector<std::string>& node_types,
                const std::vector<std::string>& edge_types,SnapshotOptions options) const
        {
            const auto owner=findHandle(graph_name);
            if (!owner || !owner->live_graph) return nullptr;

            // snapshot با shared_lock (سریع — فقط copy ساختار)
            std::shared_lock sh(owner->rw_mutex);
            std::lock_guard cache_lock(owner->cache_mutex);
            auto& handle=*owner;
            if (options.use_cache) {
                auto nodes=node_types,edges=edge_types;
                std::sort(nodes.begin(),nodes.end()); nodes.erase(std::unique(nodes.begin(),nodes.end()),nodes.end());
                std::sort(edges.begin(),edges.end()); edges.erase(std::unique(edges.begin(),edges.end()),edges.end());
                const auto signature=nlohmann::json{{"nodes",nodes},{"edges",edges}}.dump();
                if (handle.snapshot_cache && handle.snapshot_filter_signature==signature &&
                    handle.snapshot_cache->snapshotVersion()==handle.live_graph->version()) {
                    auto result=handle.snapshot_cache->clone(options); result->cache_hit_=true; return result;
                }
                handle.snapshot_cache.reset();
                auto build_options=options; build_options.incoming=false;
                auto candidate=std::make_unique<StaticGraph>(*handle.live_graph,nodes,edges,graph_name,build_options);
                auto result=candidate->clone(options);
                handle.snapshot_cache=std::move(candidate); handle.snapshot_filter_signature=signature;
                return result;
            }
            handle.snapshot_cache.reset();
            return std::make_unique<StaticGraph>(
                    *handle.live_graph,node_types,edge_types,graph_name,std::move(options));
        }

        algorithms::AlgoResult GraphManager::runLock(
                const std::string& graph_name,
                algorithms::LockAlgorithm& algo,
                const std::vector<ExtId>& params) const
        {
            const auto owner=findHandle(graph_name);
            if (!owner)
                return {false, "Graph '" + graph_name + "' not found", "", 0.0};
            std::shared_lock graph_lock(owner->rw_mutex);
            if (!owner->live_graph)
                return {false, "Graph '" + graph_name + "' has no backing graph", "", 0.0};
            if (!owner->built)
                return {false, "Graph '" + graph_name + "' is not built/rendered", "", 0.0};

            try {
                return algo.run(*owner->live_graph, params);
            } catch (const std::exception& ex) {
                return {false, std::string("LockAlgorithm '") + algo.name() +
                               "' failed: " + ex.what(), "", 0.0};
            } catch (...) {
                return {false, std::string("LockAlgorithm '") + algo.name() +
                               "' failed with unknown exception", "", 0.0};
            }
        }

        JobHandle GraphManager::submitJob(
                const std::string& graph_name,
                algorithms::JobAlgorithm& algo,
                const std::vector<ExtId>& params,SnapshotOptions options) const
        {
            // Legacy borrowed API: caller must keep algo alive through completion.
            return submitJob(graph_name,std::shared_ptr<algorithms::JobAlgorithm>(&algo,[](auto*) {}),params,std::move(options));
        }

        JobHandle GraphManager::submitJob(const std::string& graph_name,
                std::shared_ptr<algorithms::JobAlgorithm> algo,
                const std::vector<ExtId>& params,SnapshotOptions options) const
        {
            if (!options.cancellation) options.cancellation=std::make_shared<std::atomic<bool>>(false);
            const auto cancellation=options.cancellation;
            if (!algo) return JobHandle(readyAlgoFuture({false,"null job algorithm","",0}),cancellation);
            if (!options.max_build_bytes) options.max_build_bytes=64*1024*1024;
            if (options.workspace_reservation_bytes>SIZE_MAX-options.max_build_bytes)
                return JobHandle(readyAlgoFuture({false,"job reservation overflow","",0}),cancellation);
            if (options.timeout_ms) options.deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(options.timeout_ms);
            const auto reservation=options.max_build_bytes+options.workspace_reservation_bytes;
            auto future=scheduler_->submit([this,graph_name,algo=std::move(algo),params,options]() mutable {
                auto snapshot=createSnapshot(graph_name,{},{},options);
                if (!snapshot) return algorithms::AlgoResult{false,"graph not found for job","",0};
                auto result=algo->run(*snapshot,params); snapshot->checkCancelled();
                if (options.max_result_bytes && result.result_json.size()>options.max_result_bytes)
                    return algorithms::AlgoResult{false,"job result byte budget exceeded","",0};
                return result;
            },cancellation,options.job_priority,reservation,std::chrono::milliseconds(options.enqueue_timeout_ms),options.deadline);
            return JobHandle(std::move(future),cancellation);
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
            std::vector<std::shared_ptr<GraphHandle>> owners;
            {
                std::lock_guard lock(registry_mutex_);
                for (const auto& [name,handle]:graphs_) owners.push_back(handle);
            }
            for (auto& handle:owners) {
                std::unique_lock operation_lock(handle->rw_mutex);
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
                                     if (nm) buildNodeFromDoc(*h.live_graph, *nm, bson);
                                     if (em) buildEdgeFromDoc(*h.live_graph, *em, bson);
                                 });
        }

        void GraphManager::onDocumentUpdated(const std::string& collection,
                                             const std::string& old_bson,
                                             const std::string& new_bson) {
            dispatchToLiveGraphs(collection,
                                 [&](GraphHandle& h, const NodeMappingDef* nm, const EdgeMappingDef* em) {
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
            auto owner=findHandle(graph_name);
            if (!owner || !owner->live_graph) return {};
            return owner->live_graph->stats();
        }

        bool GraphManager::isReady(const std::string& graph_name) const {
            auto owner=findHandle(graph_name);
            return owner && owner->built;
        }

        GraphManager::WalStatus GraphManager::getWalStatus(
                const std::string& graph_name) const
        {
            auto owner=findHandle(graph_name);
            if (!owner || !owner->wal) return {};
            WalStatus ws;
            ws.total_entries   = owner->wal->totalEntries();
            ws.has_pending     = owner->wal->hasPendingEntries();
            ws.pending_entries = owner->wal->loadUnapplied().size();
            return ws;
        }

        std::vector<WalRecord> GraphManager::getRecentWalEntries(
                const std::string& graph_name,
                size_t limit) const
        {
            auto owner=findHandle(graph_name);
            if (!owner || !owner->wal) return {};
            return owner->wal->loadRecent(limit);
        }

        size_t GraphManager::purgeWAL(const std::string& graph_name) {
            auto owner=findHandle(graph_name);
            if (!owner || !owner->wal) return 0;
            return owner->wal->purgeOld();
        }

    } // namespace graph
} // namespace nexora
