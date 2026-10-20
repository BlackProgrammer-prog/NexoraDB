#include "TestTempDir.h"
#include "graph/BulkGraphBuilder.h"
#include "graph/StaticGraph.h"
#include "graph/GraphManager.h"
#include "graph/algorithms/BetweennessCentrality.h"
#include "graph/algorithms/BuiltinAlgorithms.h"
#include "graph/algorithms/FriendSuggestion.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <thread>

using namespace nexora::graph;

TEST(GraphSnapshot, DenseRemapLazyCscUndirectedAndSelfLoop) {
    LiveGraph graph(nullptr,nullptr,"source");
    graph.addNode("a","User"); graph.addNode("hole","Other"); graph.addNode("b","User");
    ASSERT_TRUE(graph.removeNode("hole")); graph.addEdge("a","b","knows",false);
    graph.addEdge("a","a","self",false);
    StaticGraph snapshot(graph); EXPECT_EQ(snapshot.nodeCount(),2U); EXPECT_EQ(snapshot.edgeCount(),2U);
    EXPECT_EQ(snapshot.denseId("b"),1U); EXPECT_EQ(snapshot.originalId(1),2U);
    EXPECT_FALSE(snapshot.hasIncomingIndex()); EXPECT_EQ(snapshot.outDegree(0),2U); EXPECT_EQ(snapshot.inDegree(0),2U);
    EXPECT_FALSE(snapshot.hasIncomingIndex()); EXPECT_EQ(snapshot.outgoing(1).front().neighbor,0U);
    const auto csr_bytes=snapshot.compressedBytes(); EXPECT_EQ(snapshot.incoming(1).size(),1U);
    EXPECT_TRUE(snapshot.hasIncomingIndex()); EXPECT_GT(snapshot.compressedBytes(),csr_bytes);
    EXPECT_EQ(snapshot.exportCOO().src.size(),2U); EXPECT_EQ(snapshot.exportCSR().colIdx.size(),3U);
    graph.addNode("later","User"); EXPECT_EQ(snapshot.nodeCount(),2U);
    EXPECT_NE(graph.version(),snapshot.snapshotVersion());
}

TEST(GraphSnapshot, FilteredDegreesTypeAlignmentAndOriginalIdExports) {
    LiveGraph graph(nullptr,nullptr,"source");
    graph.addNode("a","Other"); graph.addNode("b","User"); graph.addNode("c","User");
    graph.addEdge("b","c","Y"); graph.addEdge("a","c","X");
    StaticGraph all(graph); const auto csr=all.exportCSR();
    ASSERT_EQ(csr.colIdx.size(),2U);
    EXPECT_EQ(all.edgeTypeName(csr.edgeTypeIds[0]),"X"); EXPECT_EQ(all.edgeTypeName(csr.edgeTypeIds[1]),"Y");
    StaticGraph filtered(graph,{"User"},{},"filtered");
    EXPECT_EQ(filtered.nodeCount(),2U); EXPECT_EQ(filtered.edgeCount(),1U);
    EXPECT_EQ(filtered.outDegree(0),1U); EXPECT_EQ(filtered.inDegree(1),1U);
    EXPECT_EQ(filtered.exportCSR().originalNodeIds,(std::vector<uint64_t>{1,2}));
    GraphExportOptions options; options.remapNodeIdsToContiguous=false;
    const auto raw=filtered.exportCSR(options); EXPECT_EQ(raw.rowPtr,(std::vector<uint64_t>{0,0,1,1}));
    EXPECT_EQ(raw.colIdx,(std::vector<uint64_t>{2}));
    options.nodeTypes={"missing"}; EXPECT_EQ(filtered.exportCSR(options).rowPtr,(std::vector<uint64_t>{0}));
    StaticGraph unknown(graph,{}, {"missing"},"unknown"); EXPECT_EQ(unknown.edgeCount(),0U);
    EXPECT_EQ(unknown.inDegree(1),0U);
}

TEST(GraphSnapshot, CancellationMemoryAdmissionAndCallbackEarlyExit) {
    LiveGraph graph(nullptr,nullptr,"source"); graph.addEdge("a","b","link",false);
    SnapshotOptions options; options.max_build_bytes=1;
    EXPECT_THROW(StaticGraph(graph,{},{},"small",options),std::length_error);
    options.max_build_bytes=0; options.cancellation=std::make_shared<std::atomic<bool>>(true);
    EXPECT_THROW(StaticGraph(graph,{},{},"cancelled",options),std::runtime_error);
    options.cancellation->store(false); StaticGraph snapshot(graph,{},{},"valid",options);
    size_t visits=0; snapshot.forEachNeighbor(0,Direction::Both,[&](auto,auto) { ++visits; return false; });
    EXPECT_EQ(visits,1U); EXPECT_FALSE(snapshot.hasIncomingIndex());
    const auto planned=snapshot.plannedPeakBytes(); options.max_build_bytes=planned;
    StaticGraph exact(graph,{},{},"valid",options); EXPECT_EQ(exact.incoming(0).size(),1U);
    snapshot.cancel(); EXPECT_THROW(snapshot.incoming(0),std::runtime_error);
    EXPECT_FALSE(snapshot.hasIncomingIndex()); EXPECT_THROW(snapshot.exportCSR(),std::runtime_error);
}

TEST(GraphSnapshot, ConcurrentLazyCscPublicationAndConsistentSourceEpoch) {
    LiveGraph graph(nullptr,nullptr,"source"); BulkGraphBuilder builder(graph);
    for (size_t i=0;i<100;++i) builder.addEdge(std::to_string(i),std::to_string(i+1),"link");
    builder.flush(); StaticGraph snapshot(graph);
    std::atomic<size_t> count=0; std::vector<std::thread> readers;
    for (int i=0;i<4;++i) readers.emplace_back([&] {
        for (size_t node=0;node<snapshot.nodeCount();++node) count+=snapshot.incoming(node).size();
    });
    for (auto& reader:readers) reader.join();
    EXPECT_EQ(count.load(),400U);
    std::thread writer([&] { for (size_t i=101;i<180;++i) graph.addEdge(std::to_string(i),"0","link"); });
    for (int i=0;i<8;++i) {
        StaticGraph epoch(graph); size_t edges=0;
        epoch.forEachEdge([&](auto,auto src,auto dst,auto) { EXPECT_TRUE(epoch.hasNode(src)); EXPECT_TRUE(epoch.hasNode(dst)); ++edges; return true; });
        EXPECT_EQ(edges,epoch.edgeCount());
    }
    writer.join();
}

TEST(GraphAnalytics, BrandesPathReferenceAndParallelEdgeDedup) {
    LiveGraph graph(nullptr,nullptr,"source");
    graph.addEdge("a","b","link"); graph.addEdge("b","c","link"); graph.addEdge("c","d","link");
    graph.addEdge("a","b","other");
    StaticGraph snapshot(graph); algorithms::BetweennessCentrality algorithm;
    const auto result=algorithm.run(snapshot,{"2"}); ASSERT_TRUE(result.success) << result.error_msg;
    const auto json=nlohmann::json::parse(result.result_json);
    ASSERT_EQ(json["nodes"].size(),2U);
    EXPECT_NEAR(json["nodes"][0]["betweenness"].get<double>(),2.0/3,1e-6);
    EXPECT_NEAR(json["nodes"][1]["betweenness"].get<double>(),2.0/3,1e-6);
    EXPECT_EQ(json["nodes"][0]["user_id"],"b");
    snapshot.cancel(); EXPECT_THROW(algorithm.run(snapshot,{}),std::runtime_error);
}

TEST(GraphAnalytics, ReusedBfsCommunityAndMonteCarloWorkspaces) {
    TestTempDir temp("snapshot_algorithms"); nexora::core::DocEngine engine((temp.path()/"db").string());
    GraphManager manager(&engine,(temp.path()/"graphs").string()); GraphDefinition def; def.name="social";
    ASSERT_TRUE(manager.createGraph(def)); ASSERT_TRUE(manager.buildGraph("social").success);
    auto* graph=manager.getLiveGraph("social"); ASSERT_TRUE(graph);
    graph->addEdge("a","b","link",false); graph->addEdge("b","c","link",false);
    const auto result=algorithms::runAllDistances(manager,"social",{"a","all"});
    ASSERT_TRUE(result.success) << result.error_msg;
    const auto distances=nlohmann::json::parse(result.result_json);
    ASSERT_EQ(distances["sources"].size(),3U);
    for (const auto& source:distances["sources"]) {
        EXPECT_EQ(source["reached"],2U);
        if (source["source"]=="a") {
            EXPECT_EQ(source["distances"][0]["id"],"b"); EXPECT_EQ(source["distances"][0]["distance"],1);
            EXPECT_EQ(source["distances"][1]["id"],"c"); EXPECT_EQ(source["distances"][1]["distance"],2);
        }
    }
    const auto community=algorithms::runCommunityDetection(manager,"social",{"20","1","members"});
    ASSERT_TRUE(community.success) << community.error_msg;
    EXPECT_EQ(nlohmann::json::parse(community.result_json)["total_nodes_assigned"],3U);
    const auto influence=algorithms::runInfluenceMaximization(manager,"social",{"1","5","1"});
    ASSERT_TRUE(influence.success) << influence.error_msg;
    EXPECT_DOUBLE_EQ(nlohmann::json::parse(influence.result_json)["estimated_reach"].get<double>(),3.0);
    SnapshotOptions cancelled; cancelled.cancellation=std::make_shared<std::atomic<bool>>(true);
    EXPECT_FALSE(algorithms::runAllDistances(manager,"social",{"a"},cancelled).success);
    EXPECT_FALSE(algorithms::runCommunityDetection(manager,"social",{},cancelled).success);
}

TEST(GraphAnalytics, JobHandleCancellationAndBuildFailureAreResults) {
    TestTempDir temp("snapshot_job_cancel"); nexora::core::DocEngine engine((temp.path()/"db").string());
    GraphManager manager(&engine,(temp.path()/"graphs").string()); GraphDefinition def; def.name="social";
    ASSERT_TRUE(manager.createGraph(def)); ASSERT_TRUE(manager.buildGraph("social").success);
    class WaitJob final:public algorithms::JobAlgorithm {
    public:
        std::promise<void> started;
        std::string name() const override { return "WaitJob"; }
        algorithms::AlgoResult run(const StaticGraph& snapshot,const std::vector<ExtId>&) override {
            started.set_value(); for (;;) { snapshot.checkCancelled(); std::this_thread::yield(); }
        }
    } algorithm;
    auto started=algorithm.started.get_future(); auto handle=manager.submitJob("social",algorithm,{});
    started.wait(); handle.cancel(); const auto result=handle.result();
    EXPECT_FALSE(result.success); EXPECT_NE(result.error_msg.find("cancelled"),std::string::npos);
    SnapshotOptions options; options.max_build_bytes=1;
    const auto failed=manager.submitJob("social",algorithm,{},options).result(); EXPECT_FALSE(failed.success);
}

TEST(GraphSnapshot, BoundedOptInCacheVersionCanonicalFiltersAndCancellationIsolation) {
    TestTempDir temp("snapshot_cache"); nexora::core::DocEngine engine((temp.path()/"db").string());
    GraphManager manager(&engine,(temp.path()/"graphs").string()); GraphDefinition def; def.name="social";
    ASSERT_TRUE(manager.createGraph(def)); ASSERT_TRUE(manager.buildGraph("social").success);
    auto* graph=manager.getLiveGraph("social"); graph->addEdge("a","b","link");
    graph->addNode("a","User"); SnapshotOptions options; options.use_cache=true;
    auto first=manager.createSnapshot("social",{},{"link","missing"},options); EXPECT_FALSE(first->cacheHit());
    auto second=manager.createSnapshot("social",{},{"missing","link","link"},options); EXPECT_TRUE(second->cacheHit());
    EXPECT_EQ(first->snapshotVersion(),second->snapshotVersion());
    first->cancel(); EXPECT_EQ(second->outgoing(0).size(),1U);
    auto third=manager.createSnapshot("social",{},{"link","missing"},options);
    EXPECT_TRUE(third->cacheHit()); EXPECT_EQ(third->edgeCount(),1U);
    graph->addNode("b","User"); // implicit promotion must invalidate the version
    auto promoted=manager.createSnapshot("social",{},{"link","missing"},options);
    EXPECT_FALSE(promoted->cacheHit()); EXPECT_GT(promoted->snapshotVersion(),second->snapshotVersion());
    auto filtered=manager.createSnapshot("social",{"User"},{"missing"},options);
    EXPECT_FALSE(filtered->cacheHit()); EXPECT_EQ(filtered->edgeCount(),0U);
    auto hit=manager.createSnapshot("social",{"User"},{"missing"},options); EXPECT_TRUE(hit->cacheHit());
    options.max_build_bytes=hit->plannedPeakBytes()-1;
    EXPECT_THROW(manager.createSnapshot("social",{"User"},{"missing"},options),std::length_error);
    options.max_build_bytes=0; options.use_cache=false;
    EXPECT_FALSE(manager.createSnapshot("social",{"User"},{"missing"},options)->cacheHit());
}

TEST(GraphAnalytics, LimitedSuggestionMatchesFullRankingPrefix) {
    LiveGraph graph(nullptr,nullptr,"source");
    graph.addEdge("u","f1","friend",false); graph.addEdge("u","f2","friend",false);
    graph.addEdge("f1","x","friend",false); graph.addEdge("f2","x","friend",false);
    graph.addEdge("f1","y","friend",false);
    algorithms::FriendSuggestion algorithm;
    const auto full=algorithm.run(graph,{"u","100"}),limited=algorithm.run(graph,{"u","1"});
    ASSERT_TRUE(full.success); ASSERT_TRUE(limited.success);
    const auto full_json=nlohmann::json::parse(full.result_json),small_json=nlohmann::json::parse(limited.result_json);
    ASSERT_EQ(small_json["suggestions"].size(),1U);
    EXPECT_EQ(small_json["suggestions"][0],full_json["suggestions"][0]);
    EXPECT_EQ(small_json["suggestions"][0]["user_id"],"x");
}
