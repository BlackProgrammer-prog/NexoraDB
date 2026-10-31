#include "TestTempDir.h"
#include "graph/BulkGraphBuilder.h"
#include "graph/SnapshotMemory.h"
#include <gtest/gtest.h>
#include <random>

using namespace nexora::graph;

TEST(GraphMemory, ProcessSnapshotRequestsExcludeAdmissionBaselineAndFailedAllocations) {
    const auto bytes=SnapshotMemory::processCurrentBytes(),resources=SnapshotMemory::processResources();
    {
        SnapshotMemory resource(100,64);
        EXPECT_EQ(SnapshotMemory::processResources(),resources+1);
        auto* allocation=resource.allocate(32,alignof(std::max_align_t));
        EXPECT_EQ(resource.currentBytes(),96U);
        EXPECT_EQ(resource.ownedRequestedBytes(),32U);
        EXPECT_EQ(SnapshotMemory::processCurrentBytes(),bytes+32);
        EXPECT_THROW(static_cast<void>(resource.allocate(16,alignof(std::max_align_t))),std::length_error);
        EXPECT_EQ(SnapshotMemory::processCurrentBytes(),bytes+32);
        resource.deallocate(allocation,32,alignof(std::max_align_t));
        EXPECT_EQ(SnapshotMemory::processCurrentBytes(),bytes);
    }
    EXPECT_EQ(SnapshotMemory::processResources(),resources);
    EXPECT_THROW((SnapshotMemory(16,32)),std::length_error);
    EXPECT_EQ(SnapshotMemory::processResources(),resources);
}

TEST(GraphMemory, DenseEdgeSlotsSparseFallbackAndTombstoneIteration) {
    EdgeStore store;
    for (EdgeId id=0;id<10000;++id) {
        EdgeRecord edge{}; edge.edge_id=id; edge.flags=FLAG_ACTIVE; store[id]=edge;
    }
    EXPECT_EQ(store.size(),10000U); EXPECT_EQ(store.sparseSize(),0U);
    EdgeRecord outlier{}; outlier.edge_id=UINT64_MAX-1; outlier.flags=FLAG_ACTIVE;
    store[outlier.edge_id]=outlier;
    EXPECT_EQ(store.sparseSize(),1U); EXPECT_LT(store.slotCapacity(),20000U);
    EXPECT_EQ(store.at(outlier.edge_id).edge_id,outlier.edge_id);
    for (auto it=store.begin();it!=store.end();) {
        if (it->first%2==0) it=store.erase(it); else ++it;
    }
    EXPECT_EQ(store.size(),5000U); EXPECT_EQ(store.sparseSize(),0U);
    EXPECT_EQ(store.find(0),store.end()); EXPECT_THROW(store.at(0),std::out_of_range);
    size_t seen=0; const auto& const_store=store;
    for (const auto& [id,edge]:const_store) { ++seen; EXPECT_EQ(id,edge.edge_id); EXPECT_TRUE(edge.isActive()); }
    EXPECT_EQ(seen,store.size());
    EdgeRecord reused{}; reused.edge_id=0; reused.flags=FLAG_ACTIVE; store[0]=reused;
    EXPECT_EQ(store.size(),5001U);
    store.clear(); EXPECT_EQ(store.begin(),store.end());
}

TEST(GraphMemory, DenseConversionAndRandomizedMapEquivalence) {
    EdgeStore store; std::unordered_map<EdgeId,EdgeRecord> expected;
    std::mt19937 random(42);
    for (int step=0;step<10000;++step) {
        const EdgeId id=random()%400;
        if (random()%3) {
            EdgeRecord edge{}; edge.edge_id=id; edge.flags=FLAG_ACTIVE; edge.type_id=step;
            store[id]=edge; expected[id]=edge;
        } else {
            auto it=store.find(id); if (it!=store.end()) store.erase(it);
            expected.erase(id);
        }
        ASSERT_EQ(store.size(),expected.size());
    }
    store.optimizeDense(); EXPECT_EQ(store.sparseSize(),0U);
    for (const auto& [id,edge]:store) {
        ASSERT_TRUE(expected.count(id)); EXPECT_EQ(edge.type_id,expected.at(id).type_id);
    }
    for (const auto& [id,edge]:expected) EXPECT_EQ(store.at(id).type_id,edge.type_id);
}

TEST(GraphMemory, ReverseIdentityVectorHandlesReuseHolesAndEmptyStrings) {
    DenseIdMap map;
    EXPECT_EQ(map.getOrCreate("far",100),100U);
    EXPECT_EQ(map.getOrCreate("next"),101U);
    EXPECT_EQ(map.getOrCreate("",0),0U); EXPECT_TRUE(map.hasDense(0)); EXPECT_TRUE(map.hasExt(""));
    EXPECT_FALSE(map.hasDense(50)); EXPECT_EQ(map.get("far"),100U);
    EXPECT_THROW(map.getOrCreate("collision",100),std::invalid_argument);
    map.remove(100); EXPECT_EQ(map.get("far"),kInvalidDenseId);
    EXPECT_EQ(map.getOrCreate("reused",100),100U);
    map.remove(ExtId{}); EXPECT_FALSE(map.hasDense(0)); EXPECT_FALSE(map.hasExt(""));
    EXPECT_GE(map.reverseBytes(),102*sizeof(std::optional<ExtId>));
}

TEST(GraphMemory, InlineAdjacencyMovesCopiesSpillsAndReturnsInline) {
    NodeAdj adj;
    EXPECT_FALSE(adj.heavy_out); EXPECT_EQ(adj.out_edges.heapBytes(),0U);
    adj.addOut({1,0,0}); EXPECT_EQ(adj.out_edges.heapBytes(),0U);
    NodeAdj copied=adj; copied.addOut({2,1,0});
    EXPECT_EQ(adj.out_degree(),1U); EXPECT_EQ(copied.out_degree(),2U);
    EXPECT_GT(copied.out_edges.heapBytes(),0U);
    ASSERT_TRUE(copied.removeOut(2,0)); EXPECT_EQ(copied.out_edges.heapBytes(),0U);
    for (size_t i=2;i<1200;++i) adj.addOut({i,i,0});
    EXPECT_TRUE(adj.out_heavy); EXPECT_TRUE(adj.heavy_out);
    EXPECT_EQ(adj.out_edges.heapBytes(),0U);
    NodeAdj moved=std::move(adj),heavy_copy=moved;
    EXPECT_EQ(adj.out_degree(),0U);
    EXPECT_EQ(heavy_copy.out_degree(),moved.out_degree());
    EXPECT_TRUE(heavy_copy.removeOut(20,0)); EXPECT_NE(heavy_copy.out_degree(),moved.out_degree());
    for (size_t i=1;i<1200;++i) {
        if (i!=20) { ASSERT_TRUE(heavy_copy.removeOut(i,0)); }
    }
    EXPECT_EQ(heavy_copy.out_degree(),0U); EXPECT_FALSE(heavy_copy.out_heavy);
    EXPECT_FALSE(heavy_copy.heavy_out); EXPECT_EQ(heavy_copy.out_edges.heapBytes(),0U);
    NodeAdj incoming;
    for (size_t i=0;i<1200;++i) incoming.addIn({i,i,0});
    for (size_t i=0;i<1200;++i) ASSERT_TRUE(incoming.removeIn(i,0));
    EXPECT_FALSE(incoming.in_heavy); EXPECT_FALSE(incoming.heavy_in);
    EXPECT_EQ(incoming.in_edges.heapBytes(),0U);
}

TEST(GraphMemory, BudgetRejectsCrudAndBulkBeforeWalOrGraphMutation) {
    TestTempDir temp("graph_memory_budget"); GraphStorage storage(temp.path(),"social");
    GraphWAL wal(temp.path()/"graph.wal"); ASSERT_TRUE(storage.open()); ASSERT_TRUE(wal.open());
    LiveGraph graph(&storage,&wal,"social"); const auto baseline=graph.memoryUsage();
    graph.setMemoryBudget(baseline.total_bytes_estimate);
    EXPECT_THROW(graph.addNode("a","User"),std::length_error);
    EXPECT_THROW(graph.addEdge("a","b","link"),std::length_error);
    EXPECT_EQ(wal.lastSeq(),0U); EXPECT_EQ(graph.activeNodeCount(),0U);
    BulkGraphBuilder bulk(graph); bulk.addEdge("a","b","link");
    EXPECT_THROW(bulk.flush(),std::length_error); EXPECT_EQ(wal.lastSeq(),0U);
    graph.setMemoryBudget(baseline.total_bytes_estimate+10000); bulk.flush();
    EXPECT_EQ(graph.activeNodeCount(),2U); EXPECT_EQ(graph.activeEdgeCount(),1U);
    const auto usage=graph.memoryUsage();
    EXPECT_LE(usage.admission_charged_bytes,usage.budget_bytes);
    EXPECT_EQ(usage.adjacency_payload_bytes,0U); EXPECT_EQ(usage.inline_adjacency_lists,4U);
    EXPECT_THROW(graph.setMemoryBudget(1),std::length_error);
    EXPECT_EQ(graph.memoryUsage().budget_bytes,usage.budget_bytes);
    graph.setMemoryBudget(0); EXPECT_EQ(graph.memoryUsage().budget_bytes,0U);
    EXPECT_NE(graph.addNode("c","User"),kInvalidDenseId);
}

TEST(GraphMemory, BreakdownSumsCapacitiesAndTracksDenseStorageAcrossRestart) {
    TestTempDir temp("graph_memory_restart");
    for (int restart=0;restart<2;++restart) {
        GraphStorage storage(temp.path(),"social"); GraphWAL wal(temp.path()/"graph.wal");
        ASSERT_TRUE(storage.open()); ASSERT_TRUE(wal.open()); LiveGraph graph(&storage,&wal,"social");
        if (!restart) {
            BulkGraphBuilder bulk(graph);
            for (int i=0;i<200;++i) bulk.addEdge("hub",std::to_string(i),"link");
            bulk.flush(); ASSERT_TRUE(graph.removeEdge("hub","10","link"));
        } else { ASSERT_TRUE(graph.loadFromDisk()); EXPECT_EQ(graph.replayWAL(),0U); }
        const auto u=graph.memoryUsage();
        EXPECT_EQ(u.total_bytes_estimate,u.object_bytes+u.node_records_bytes+u.adjacency_headers_bytes+
            u.adjacency_payload_bytes+u.edge_store_bytes+u.reverse_identity_bytes+u.forward_identity_bytes_estimate+
            u.type_registry_bytes_estimate+u.free_ids_bytes_estimate+u.wal_bytes_estimate+u.storage_bytes_estimate);
        EXPECT_EQ(u.sparse_edges,0U); EXPECT_GE(u.edge_slot_capacity,200U);
        EXPECT_EQ(graph.activeEdgeCount(),199U); EXPECT_EQ(graph.getNode("hub")->out_degree,199U);
    }
}
