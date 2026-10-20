#include "TestTempDir.h"
#include "graph/BulkGraphBuilder.h"
#include "graph/GraphManager.h"
#include <gtest/gtest.h>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

using namespace nexora::graph;

TEST(GraphBulk, SortDedupDegreesPromotionAndRestart) {
    TestTempDir temp("graph_bulk_reload");
    for (int restart=0;restart<2;++restart) {
        GraphStorage storage(temp.path(),"social"); GraphWAL wal(temp.path()/"graph.wal");
        ASSERT_TRUE(storage.open()); ASSERT_TRUE(wal.open());
        LiveGraph graph(&storage,&wal,"social");
        if (!restart) {
            BulkGraphBuilder bulk(graph);
            bulk.addEdge("b","a","knows",false);
            bulk.addEdge("a","b","knows",false);
            bulk.addEdge("a","a","self",false);
            bulk.addNode("a","User"); bulk.addNode("a","Other");
            const auto before=wal.syncCount(); bulk.flush();
            EXPECT_EQ(wal.syncCount()-before,1U);
            EXPECT_EQ(bulk.stats().edges_created,2U);
            EXPECT_EQ(bulk.stats().edges_deduplicated,1U);
            EXPECT_EQ(bulk.stats().nodes_created,2U);
            // Dedup against earlier batches, and promote an implicit node.
            bulk.addEdge("b","a","knows",false); bulk.addNode("b","Person"); bulk.flush();
            EXPECT_EQ(bulk.stats().edges_created,2U);
            EXPECT_EQ(bulk.stats().edges_deduplicated,2U);
            EXPECT_FALSE(wal.hasPendingEntries());
        } else { ASSERT_TRUE(graph.loadFromDisk()); EXPECT_EQ(graph.replayWAL(),0U); }
        EXPECT_EQ(graph.activeNodeCount(),2U); EXPECT_EQ(graph.activeEdgeCount(),2U);
        ASSERT_TRUE(graph.getNode("a")); ASSERT_TRUE(graph.getNode("b"));
        EXPECT_EQ(graph.getNode("a")->type_name,"User");
        EXPECT_EQ(graph.getNode("b")->type_name,"Person");
        EXPECT_EQ(graph.getNode("a")->out_degree,2U);
        EXPECT_EQ(graph.getNode("a")->in_degree,2U);
        EXPECT_EQ(graph.neighbors(graph.getDenseId("a"),Direction::Out).size(),2U);
        EXPECT_EQ(storage.readNode(graph.getDenseId("a"))->out_degree,2U);
    }
}

TEST(GraphBulk, BoundedProducerBackpressureAndOversizeRejection) {
    LiveGraph graph(nullptr,nullptr,"memory"); BulkOptions options;
    options.max_records=2; options.max_buffer_bytes=2500;
    BulkGraphBuilder bulk(graph,options);
    for (int i=0;i<9;++i) bulk.addEdge("hub",std::to_string(i),"link");
    EXPECT_GT(bulk.stats().batches,0U); bulk.flush();
    EXPECT_LE(bulk.stats().peak_buffer_bytes,options.max_buffer_bytes);
    EXPECT_EQ(bulk.bufferedBytes(),0U); EXPECT_EQ(graph.activeEdgeCount(),9U);
    const auto version=graph.version();
    EXPECT_THROW(bulk.addNode(std::string(1000,'x'),"User"),std::length_error);
    EXPECT_EQ(graph.version(),version); EXPECT_EQ(bulk.bufferedBytes(),0U);
    EXPECT_THROW(bulk.addEdge("","a","link"),std::invalid_argument);
    options.max_records=0; EXPECT_THROW(BulkGraphBuilder(graph,options),std::invalid_argument);
}

TEST(GraphBulk, DirectedAndUndirectedKeysAndHeavyExistingAdjacency) {
    LiveGraph graph(nullptr,nullptr,"memory");
    for (int i=0;i<1100;++i) graph.addEdge("hub",std::to_string(i),"link");
    BulkGraphBuilder bulk(graph);
    for (int i=0;i<1200;++i) bulk.addEdge("hub",std::to_string(i),"link");
    bulk.flush();
    EXPECT_EQ(graph.activeEdgeCount(),1200U); EXPECT_EQ(bulk.stats().edges_deduplicated,1100U);
    EXPECT_EQ(graph.getNode("hub")->out_degree,1200U);
    EXPECT_EQ(graph.neighbors(graph.getDenseId("hub"),Direction::Out).size(),1200U);
    graph.addEdge("hub","last","link");
    EXPECT_EQ(graph.getNode("hub")->out_degree,1201U);
    EXPECT_TRUE(graph.removeEdge("hub","last","link"));
    BulkGraphBuilder mixed(graph);
    mixed.addEdge("x","y","mixed",true);
    mixed.addEdge("y","x","mixed",false);
    mixed.addEdge("y","x","mixed",true); mixed.flush();
    EXPECT_EQ(mixed.stats().edges_created,2U);
    EXPECT_EQ(mixed.stats().edges_deduplicated,1U);
    EXPECT_EQ(graph.getNode("x")->out_degree,1U);
    EXPECT_EQ(graph.getNode("y")->out_degree,1U);
}

TEST(GraphBulk, SigkillBatchReplaysCompleteDurablePrefixIdempotently) {
    for (int boundary=0;boundary<3;++boundary) {
        TestTempDir temp("graph_bulk_kill"); int notify[2]; ASSERT_EQ(::pipe(notify),0);
        const auto pid=::fork(); ASSERT_GE(pid,0);
        if (pid==0) {
            ::close(notify[0]);
            GraphStorage storage(temp.path(),"social"); GraphWAL wal(temp.path()/"graph.wal");
            if (!storage.open() || !wal.open()) ::_exit(2);
            LiveGraph graph(&storage,&wal,"social"); BulkGraphBuilder bulk(graph);
            bulk.setHookForTesting([&](BulkGraphBuilder::Point point) {
                if (int(point)!=boundary) return;
                const char ready='R'; if (::write(notify[1],&ready,1)!=1) ::_exit(3);
                for (;;) ::pause();
            });
            bulk.addNode("a","User"); bulk.addEdge("a","b","knows",false);
            bulk.addEdge("b","c","knows"); bulk.flush(); ::_exit(4);
        }
        ::close(notify[1]); char ready{}; const auto received=::read(notify[0],&ready,1); ::close(notify[0]);
        ::kill(pid,SIGKILL); int status=0; ASSERT_EQ(::waitpid(pid,&status,0),pid);
        ASSERT_EQ(received,1); ASSERT_EQ(ready,'R'); ASSERT_TRUE(WIFSIGNALED(status));
        ASSERT_EQ(WTERMSIG(status),SIGKILL);
        for (int restart=0;restart<2;++restart) {
            GraphStorage storage(temp.path(),"social"); GraphWAL wal(temp.path()/"graph.wal");
            ASSERT_TRUE(storage.open()) << storage.lastError(); ASSERT_TRUE(wal.open());
            LiveGraph graph(&storage,&wal,"social"); ASSERT_TRUE(graph.loadFromDisk());
            EXPECT_EQ(graph.replayWAL(),restart==0 && boundary<2 ? 5U : 0U);
            EXPECT_EQ(graph.activeNodeCount(),3U); EXPECT_EQ(graph.activeEdgeCount(),2U);
            EXPECT_EQ(graph.neighbors(graph.getDenseId("b"),Direction::Out).size(),2U);
            EXPECT_EQ(graph.getNode("b")->out_degree,2U); EXPECT_EQ(graph.replayWAL(),0U);
        }
    }
}

TEST(GraphBulk, WalBatchValidatesAllRecordsAndKeepsCheckpointContiguous) {
    TestTempDir temp("graph_bulk_wal"); GraphWAL wal(temp.path()/"graph.wal"); ASSERT_TRUE(wal.open());
    std::vector<WalRecord> records(4);
    for (auto& record:records) record.op=WalOpType::AddNode;
    records.back().op=static_cast<WalOpType>(255);
    EXPECT_TRUE(wal.appendBatch(records).empty()); EXPECT_EQ(wal.lastSeq(),0U);
    records.back().op=WalOpType::AddNode; const auto before=wal.syncCount();
    const auto seq=wal.appendBatch(records); ASSERT_EQ(seq.size(),4U);
    EXPECT_EQ(wal.syncCount()-before,1U);
    EXPECT_FALSE(wal.markAppliedBatch({seq[0],999})); EXPECT_EQ(wal.appliedLsn(),0U);
    ASSERT_TRUE(wal.markAppliedBatch({seq[2],seq[3]})); EXPECT_EQ(wal.appliedLsn(),0U);
    ASSERT_TRUE(wal.markAppliedBatch({seq[0],seq[1]})); EXPECT_EQ(wal.appliedLsn(),4U);
    wal.close(); ASSERT_TRUE(wal.open()); EXPECT_TRUE(wal.loadUnapplied().empty());
}

TEST(GraphBulk, PoliciesRotationAndDeletedSlotReuse) {
    for (const auto policy:{WalDurability::Sync,WalDurability::GroupCommit,WalDurability::Async}) {
        TestTempDir temp("graph_bulk_policies"); WalOptions options; options.durability=policy;
        options.segment_bytes=GraphWAL::kRecordBytes*2;
        {
            GraphStorage storage(temp.path(),"social"); GraphWAL wal(temp.path()/"graph.wal",86400,options);
            ASSERT_TRUE(storage.open()); ASSERT_TRUE(wal.open()); LiveGraph graph(&storage,&wal,"social");
            auto id=graph.addEdge("old","other","link"); ASSERT_TRUE(graph.removeEdge(id));
            ASSERT_TRUE(graph.removeNode("old"));
            BulkOptions limits; limits.max_records=2; BulkGraphBuilder bulk(graph,limits);
            bulk.addNode("a","User"); bulk.addEdge("a","b","link");
            bulk.addEdge("b","c","link",false); bulk.flush();
            EXPECT_EQ(graph.activeNodeCount(),4U); EXPECT_EQ(graph.activeEdgeCount(),2U);
            ASSERT_TRUE(graph.checkpointWAL());
        }
        GraphStorage storage(temp.path(),"social"); GraphWAL wal(temp.path()/"graph.wal",86400,options);
        ASSERT_TRUE(storage.open()); ASSERT_TRUE(wal.open()); LiveGraph graph(&storage,&wal,"social");
        ASSERT_TRUE(graph.loadFromDisk()); EXPECT_EQ(graph.replayWAL(),0U);
        EXPECT_FALSE(graph.hasNode("old")); EXPECT_EQ(graph.activeEdgeCount(),2U);
        EXPECT_EQ(graph.getNode("b")->out_degree,1U); EXPECT_EQ(graph.getNode("b")->in_degree,2U);
    }
}

TEST(GraphBulk, FailurePoisonsWriterAndReopenRecoversDurableRedo) {
    TestTempDir temp("graph_bulk_failure");
    {
        GraphStorage storage(temp.path(),"social"); GraphWAL wal(temp.path()/"graph.wal");
        ASSERT_TRUE(storage.open()); ASSERT_TRUE(wal.open()); LiveGraph graph(&storage,&wal,"social");
        BulkGraphBuilder bulk(graph);
        bulk.setHookForTesting([](auto point) {
            if (point==BulkGraphBuilder::Point::WalDurable) throw std::runtime_error("injected failure");
        });
        bulk.addEdge("a","b","link"); EXPECT_THROW(bulk.flush(),std::runtime_error);
        EXPECT_THROW(bulk.flush(),std::runtime_error);
        EXPECT_THROW(graph.addNode("c","User"),std::runtime_error);
        BulkGraphBuilder other(graph); other.addNode("d","User"); EXPECT_THROW(other.flush(),std::runtime_error);
        EXPECT_THROW(graph.checkpointWAL(),std::runtime_error);
    }
    GraphStorage storage(temp.path(),"social"); GraphWAL wal(temp.path()/"graph.wal");
    ASSERT_TRUE(storage.open()); ASSERT_TRUE(wal.open()); LiveGraph graph(&storage,&wal,"social");
    ASSERT_TRUE(graph.loadFromDisk()); EXPECT_EQ(graph.replayWAL(),3U);
    EXPECT_EQ(graph.activeEdgeCount(),1U); EXPECT_FALSE(graph.hasNode("c"));
}

TEST(GraphBulk, ManagerBuildConsumesBsonMappingsAndReportsDeduplicatedCounts) {
    TestTempDir temp("graph_bulk_manager");
    nexora::core::DocEngine engine((temp.path()/"db").string());
    ASSERT_TRUE(engine.CreateCollection("users").success);
    ASSERT_TRUE(engine.InsertOne("users",R"({"_id":"a","following":["b","b","c"]})").success);
    ASSERT_TRUE(engine.InsertOne("users",R"({"_id":"b","following":[]})").success);
    GraphManager manager(&engine,(temp.path()/"graphs").string());
    GraphDefinition def; def.name="social";
    NodeMappingDef node; node.collection="users"; node.node_type="User"; node.key_path="_id";
    def.node_mappings.push_back(node);
    EdgeMappingDef edge; edge.collection="users"; edge.edge_type="follows";
    edge.source_path="_id"; edge.target_path="target"; edge.unwind=UnwindConfig{"following","target"};
    def.edge_mappings.push_back(edge);
    ASSERT_TRUE(manager.createGraph(def)); const auto result=manager.buildGraph("social");
    ASSERT_TRUE(result.success) << result.error_msg;
    EXPECT_EQ(result.nodes_built,3U); EXPECT_EQ(result.edges_built,2U);
    EXPECT_TRUE(manager.isReady("social")); EXPECT_FALSE(manager.getWalStatus("social").has_pending);
    EXPECT_EQ(manager.getStats("social").active_edges,2U);
}
