#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "core/security/SecureDatabase.h"
namespace py=pybind11;
using namespace nexora::security;
#ifdef NEXORA_BUILD_GRAPH
#include "graph/GraphManager.h"
#include "graph/algorithms/BuiltinAlgorithms.h"
namespace nexora::security {
class GraphGateway final {
    std::shared_ptr<SecureDatabase> database_;
    std::shared_ptr<graph::GraphManager> manager_;
public:
    GraphGateway(std::shared_ptr<SecureDatabase> db,const std::string& dir,const std::string& token)
        :database_(std::move(db)) {
        py::gil_scoped_release release;
        // Startup is a fixed internal maintenance task. No data/manager handle
        // is exposed; every user-facing operation below requires authorization.
        if(!token.empty()) {auto lease=database_->authority_->authorize(token,"graph");}
        manager_=std::make_shared<graph::GraphManager>(database_->engine_.get(),dir);
        if(!manager_->startup()) throw std::runtime_error("Graph recovery failed");
        std::weak_ptr<graph::GraphManager> weak=manager_;
        std::lock_guard lock(database_->projection_mutex_);
        database_->refresh_projection_=[weak](const std::string& collection) {
            if(auto manager=weak.lock()) for(const auto& name:manager->listGraphs()) {
                const auto definition=manager->getDefinition(name);if(!definition) continue;
                bool mapped=false;
                for(const auto& mapping:definition->node_mappings) mapped |= mapping.collection==collection;
                for(const auto& mapping:definition->edge_mappings) mapped |= mapping.collection==collection;
                if(mapped && !manager->buildGraph(name).success)
                    throw std::runtime_error("Document committed but graph projection refresh failed; reconcile before retrying");
            }
        };
    }
    bool prepareShutdown(){py::gil_scoped_release release;return manager_->prepareShutdown();}
    void shutdown(){py::gil_scoped_release release;manager_->shutdown();}
    py::object call(const std::string& token,const std::string& op,const py::list& args) {
        const auto run=[&](auto fn) {
            py::gil_scoped_release release;
            auto lease=database_->authority_->authorize(token,"graph");
            auto deadline=core::current_execution_control?core::current_execution_control:std::make_shared<core::ExecutionControl>(30000);
            core::ExecutionGuard guard(deadline);
            return fn(*manager_);
        };
        const auto name=[&]{return args[0].cast<std::string>();};
        if(op=="list_graphs") return py::cast(run([](auto& gm){return gm.listGraphs();}));
        if(op=="startup") return py::cast(run([](auto&){return true;}));
        if(op=="create_graph") {auto def=args[0].cast<graph::GraphDefinition>();return py::cast(run([&](auto& gm){return gm.createGraph(def);}));}
        if(op=="drop_graph") {auto n=name();return py::cast(run([&](auto& gm){return gm.dropGraph(n);}));}
        if(op=="get_definition") {auto n=name();return py::cast(run([&](auto& gm){return gm.getDefinition(n);}));}
        if(op=="get_stats") {auto n=name();return py::cast(run([&](auto& gm){return gm.getStats(n);}));}
        if(op=="build_graph") {auto n=name();return py::cast(run([&](auto& gm){return gm.buildGraph(n);}));}
        if(op=="render_graph") {auto n=name();return py::cast(run([&](auto& gm){return gm.renderGraph(n);}));}
        if(op=="checkpoint_graph") {auto n=name();return py::cast(run([&](auto& gm){return gm.checkpointGraph(n);}));}
        if(op=="add_node_mapping") {auto n=name();auto mapping=args[1].cast<graph::NodeMappingDef>();return py::cast(run([&](auto& gm){return gm.addNodeMapping(n,mapping);}));}
        if(op=="add_edge_mapping") {auto n=name();auto mapping=args[1].cast<graph::EdgeMappingDef>();return py::cast(run([&](auto& gm){return gm.addEdgeMapping(n,mapping);}));}
        if(op=="create_snapshot") {
            auto n=name();auto result=run([&](auto& gm){return gm.createSnapshot(n);});
            return result?py::cast(std::move(result)):py::none();
        }
        if(op=="neighbors") {
            const auto n=name(),id=args[1].cast<std::string>();
            const auto direction=py::len(args)>2?args[2].cast<std::string>():"out";
            const auto type=py::len(args)>3?args[3].cast<std::string>():"";
            const auto limit=py::len(args)>4?args[4].cast<size_t>():100;
            if(limit>10000) throw std::invalid_argument("Traversal limit exceeds 10000");
            auto dir=graph::Direction::Out;if(direction=="in" || direction=="IN") dir=graph::Direction::In;
            if(direction=="both" || direction=="BOTH") dir=graph::Direction::Both;
            return py::cast(run([&](auto& gm){auto [g,lock]=gm.acquireReadLock(n);
                return g?g->neighborsExt(id,dir,type,limit):std::vector<std::string>{};}));
        }
        if(op=="has_edge") {
            const auto n=name(),src=args[1].cast<std::string>(),dst=args[2].cast<std::string>();
            const auto type=py::len(args)>3?args[3].cast<std::string>():"";
            return py::cast(run([&](auto& gm){auto [g,lock]=gm.acquireReadLock(n);return g && g->hasEdge(src,dst,type);}));
        }
        if(op=="get_wal_status") {
            const auto n=name();const auto status=run([&](auto& gm){return gm.getWalStatus(n);});
            py::dict value;value["total_entries"]=status.total_entries;value["pending_entries"]=status.pending_entries;
            value["has_pending"]=status.has_pending;return value;
        }
        if(op=="on_document_inserted" || op=="on_document_deleted" || op=="on_document_updated") {
            // Projection has already refreshed from committed native data.
            // Never trust caller-supplied old/new documents for graph mutation.
            run([](auto&){return true;});
            return py::none();
        }
        if(op.starts_with("run_")) {
            const auto n=name();const auto params=py::len(args)>1?args[1].cast<std::vector<std::string>>():std::vector<std::string>{};
#define SECURE_ALGO(operation, function) if(op==operation) return py::cast(run([&](auto& gm){return graph::algorithms::function(gm,n,params);}));
            SECURE_ALGO("run_mutual_friends",runMutualFriends)
            SECURE_ALGO("run_network_stats",runNetworkStats)
            SECURE_ALGO("run_most_connected",runMostConnected)
            SECURE_ALGO("run_get_friends",runGetFriends)
            SECURE_ALGO("run_are_connected",runAreConnected)
            SECURE_ALGO("run_shortest_path",runShortestPath)
            SECURE_ALGO("run_friend_suggestion",runFriendSuggestion)
            SECURE_ALGO("run_connected_components",runConnectedComponents)
            SECURE_ALGO("run_community_detection",runCommunityDetection)
            SECURE_ALGO("run_all_distances",runAllDistances)
            SECURE_ALGO("run_betweenness_centrality",runBetweennessCentrality)
            SECURE_ALGO("run_influence_maximization",runInfluenceMaximization)
#undef SECURE_ALGO
        }
        throw AuthorizationError("Unsupported operation at native graph security boundary");
    }
};
}
#endif
void bind_secure_database(py::module_& m) {
    py::register_exception<AuthenticationError>(m,"AuthenticationError",PyExc_RuntimeError);
    py::register_exception<AuthorizationError>(m,"AuthorizationError",PyExc_RuntimeError);
    py::register_exception<RateLimitError>(m,"AuthenticationRateLimited",PyExc_RuntimeError);
    py::class_<SessionConfig>(m,"SessionConfig")
        .def(py::init<>())
        .def_readwrite("idle_seconds",&SessionConfig::idle_seconds)
        .def_readwrite("absolute_seconds",&SessionConfig::absolute_seconds)
        .def_readwrite("max_sessions",&SessionConfig::max_sessions);
    py::class_<DataRequest>(m,"SecureDataRequest")
        .def(py::init<>())
        .def_readwrite("operation",&DataRequest::operation)
        .def_readwrite("collection",&DataRequest::collection)
        .def_readwrite("id",&DataRequest::id)
        .def_readwrite("payload",&DataRequest::payload)
        .def_readwrite("page_token",&DataRequest::page_token)
        .def_readwrite("target_collection",&DataRequest::target_collection)
        .def_readwrite("source_field",&DataRequest::source_field)
        .def_readwrite("target_field",&DataRequest::target_field)
        .def_readwrite("condition",&DataRequest::condition)
        .def_readwrite("update",&DataRequest::update)
        .def_readwrite("schema",&DataRequest::schema)
        .def_readwrite("index",&DataRequest::index)
        .def_readwrite("foreign_key",&DataRequest::foreign_key)
        .def_readwrite("documents",&DataRequest::documents)
        .def_readwrite("joins",&DataRequest::joins)
        .def_readwrite("limit",&DataRequest::limit)
        .def_readwrite("skip",&DataRequest::skip)
        .def_readwrite("max_bytes",&DataRequest::max_bytes)
        .def_readwrite("flag",&DataRequest::flag)
        .def_readwrite("second_flag",&DataRequest::second_flag)
        .def_readwrite("transaction",&DataRequest::transaction);
    py::class_<SecureTransaction,std::shared_ptr<SecureTransaction>>(m,"SecureTransaction")
        .def("is_valid",&SecureTransaction::isValid);
    py::class_<SecureDatabase,std::shared_ptr<SecureDatabase>>(m,"SecureDatabase")
        .def(py::init<const std::string&,const nexora::core::RocksSettings&,SessionConfig>(),
            py::arg("path"),py::arg("settings")=nexora::core::RocksSettings{},py::arg("sessions")=SessionConfig{})
        .def("login",&SecureDatabase::login,py::arg("username"),py::arg("password"),py::arg("source")="native",
            py::call_guard<py::gil_scoped_release>())
        .def("claims",&SecureDatabase::claims,py::call_guard<py::gil_scoped_release>())
        .def("logout",&SecureDatabase::logout,py::call_guard<py::gil_scoped_release>())
        .def("change_password",&SecureDatabase::changePassword,py::call_guard<py::gil_scoped_release>())
        .def("complete_reset",&SecureDatabase::completeReset,py::call_guard<py::gil_scoped_release>())
        .def("administer",&SecureDatabase::administer,py::call_guard<py::gil_scoped_release>())
        .def("execute",&SecureDatabase::execute,py::call_guard<py::gil_scoped_release>())
        .def("ready",&SecureDatabase::ready)
        .def("prepare_shutdown",&SecureDatabase::prepareShutdown,py::call_guard<py::gil_scoped_release>());
    // Local service drain is not a data-access capability. Host code can
    // always release the owning object; no HTTP shutdown endpoint is exposed.
}

void bind_secure_graph(py::module_& m) {
#ifdef NEXORA_BUILD_GRAPH
    py::class_<GraphGateway,std::shared_ptr<GraphGateway>>(m,"SecureGraphGateway")
        .def(py::init<std::shared_ptr<SecureDatabase>,const std::string&,const std::string&>())
        .def("call",&GraphGateway::call)
        .def("prepare_shutdown",&GraphGateway::prepareShutdown)
        .def("shutdown",&GraphGateway::shutdown);
#endif
}
