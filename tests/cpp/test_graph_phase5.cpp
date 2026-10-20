#include "TestTempDir.h"
#include "graph/BulkGraphBuilder.h"
#include "graph/StaticGraph.h"
#include <gtest/gtest.h>
#include <map>
#include <random>
#include <tuple>

using namespace nexora::graph;
namespace {
struct Event { bool node; std::string a,b,type; bool directed=true; };
struct View {
    std::map<ExtId,std::tuple<std::string,bool,uint64_t,uint64_t>> nodes;
    std::vector<std::tuple<ExtId,ExtId,std::string,bool>> edges;
    std::map<ExtId,std::pair<std::vector<ExtId>,std::vector<ExtId>>> traversal;
    bool operator==(const View&) const=default;
};
View view(const LiveGraph& graph) {
    View result; std::vector<DenseId> ids; std::vector<EdgeRecord> edges;
    graph.forEachNode([&](auto id,const auto&) { ids.push_back(id); return true; });
    graph.forEachEdge([&](auto,const auto& edge) { edges.push_back(edge); return true; });
    for (auto id:ids) {
        const auto node=graph.getNode(id); const auto& ext=node->external_id;
        result.nodes[ext]={node->type_name,bool(node->flags&FLAG_IMPLICIT),node->out_degree,node->in_degree};
        auto out=graph.neighborsExt(ext,Direction::Out,"",SIZE_MAX),in=graph.neighborsExt(ext,Direction::In,"",SIZE_MAX);
        std::sort(out.begin(),out.end()); std::sort(in.begin(),in.end());
        result.traversal[ext]={std::move(out),std::move(in)};
    }
    for (const auto& edge:edges) {
        auto src=graph.getExtId(edge.src),dst=graph.getExtId(edge.dst); const bool directed=!(edge.flags&FLAG_UNDIRECTED);
        if (!directed && dst<src) std::swap(src,dst);
        result.edges.emplace_back(src,dst,graph.getEdgeTypeName(edge.type_id),directed);
    }
    std::sort(result.edges.begin(),result.edges.end()); return result;
}
std::vector<Event> corpus() {
    std::vector<Event> events={{false,"b","a","mixed",true},{false,"a","b","mixed",false},
        {false,"a","b","mixed",true},{false,"self","self","loop",false},
        {true,"a","","User"},{true,"b","","User"},{true,"سلام\"\\,{}","","Unicode"}};
    std::mt19937 rng(20261020);
    for (int i=0;i<300;++i) {
        auto a="n"+std::to_string(rng()%25),b="n"+std::to_string(rng()%25);
        events.push_back({false,a,b,"type"+std::to_string(rng()%3),bool(rng()%2)});
        if (i%9==0) events.push_back({true,a,"","Class"+std::to_string(rng()%2)});
    }
    return events;
}
void single(LiveGraph& graph,const std::vector<Event>& events) {
    for (const auto& e:events) {
        if (e.node) graph.addNode(e.a,e.type); else graph.addEdge(e.a,e.b,e.type,e.directed);
    }
}
void bulk(LiveGraph& graph,const std::vector<Event>& events,size_t batch) {
    BulkOptions limits; limits.max_records=batch; BulkGraphBuilder builder(graph,limits);
    for (const auto& e:events) {
        if (e.node) builder.addNode(e.a,e.type); else builder.addEdge(e.a,e.b,e.type,e.directed);
    }
    builder.flush();
}
}
TEST(GraphPhase5, BulkMatchesSingleForRandomCorpusAndEveryBatchBoundary) {
    const auto events=corpus(); LiveGraph reference(nullptr,nullptr,"single"); single(reference,events);
    const auto expected=view(reference);
    for (size_t batch:{1U,2U,7U,64U,4096U}) {
        SCOPED_TRACE(batch); LiveGraph graph(nullptr,nullptr,"bulk"); bulk(graph,events,batch);
        EXPECT_TRUE(view(graph)==expected);
        StaticGraph snapshot(graph); EXPECT_EQ(snapshot.edgeCount(),expected.edges.size());
    }
}
TEST(GraphPhase5, BulkSingleParitySurvivesDurableRestartAndAsyncRedo) {
    auto events=corpus(); events.resize(60);
    for (auto policy:{WalDurability::Sync,WalDurability::Async}) {
        TestTempDir temp("phase5_parity"); View expected;
        for (int restart=0;restart<3;++restart) {
            WalOptions options; options.durability=policy;
            GraphStorage a(temp.path(),"single"),b(temp.path(),"bulk");
            GraphWAL wa(temp.path()/"single.wal",86400,options),wb(temp.path()/"bulk.wal",86400,options);
            ASSERT_TRUE(a.open()); ASSERT_TRUE(b.open()); ASSERT_TRUE(wa.open()); ASSERT_TRUE(wb.open());
            LiveGraph ga(&a,&wa,"single"),gb(&b,&wb,"bulk");
            if (!restart) { single(ga,events); bulk(gb,events,11); expected=view(ga); }
            else { ASSERT_TRUE(ga.loadFromDisk()); ASSERT_TRUE(gb.loadFromDisk()); ga.replayWAL(); gb.replayWAL(); }
            EXPECT_TRUE(view(ga)==expected); EXPECT_TRUE(view(gb)==expected);
            if (restart) { ASSERT_TRUE(ga.checkpointWAL()); ASSERT_TRUE(gb.checkpointWAL()); }
        }
    }
}
TEST(GraphPhase5, SnapshotAllocatorRejectsBeforeExceedingRequestedByteLimit) {
    SnapshotMemory memory(128);
    {
        std::pmr::vector<uint64_t> values(&memory); values.reserve(8);
        void* extra=memory.allocate(64,alignof(uint64_t));
        EXPECT_EQ(memory.currentBytes(),128U); EXPECT_EQ(memory.peakBytes(),128U);
        EXPECT_THROW((void)memory.allocate(1,alignof(char)),std::length_error);
        EXPECT_EQ(memory.peakBytes(),128U); memory.deallocate(extra,64,alignof(uint64_t));
        EXPECT_THROW(values.reserve(16),std::length_error); EXPECT_EQ(values.capacity(),8U);
    }
    EXPECT_EQ(memory.currentBytes(),0U);
}
TEST(GraphPhase5, MeasuredSnapshotPeakIncludesStringsCscScratchAndCacheClone) {
    LiveGraph graph(nullptr,nullptr,"source"); BulkGraphBuilder builder(graph);
    for (int i=0;i<100;++i) builder.addNode(std::to_string(i)+std::string(2048,'x'),"Node");
    for (int i=0;i<99;++i) builder.addEdge(std::to_string(i)+std::string(2048,'x'),std::to_string(i+1)+std::string(2048,'x'),"link");
    builder.flush(); SnapshotOptions options; options.max_build_bytes=8*1024*1024;
    StaticGraph snapshot(graph,{},{},"bounded",options);
    EXPECT_GT(snapshot.allocationCurrentBytes(),400000U);
    EXPECT_LE(snapshot.allocationPeakBytes(),options.max_build_bytes);
    const auto before=snapshot.allocationPeakBytes(); (void)snapshot.incoming(0);
    EXPECT_GE(snapshot.allocationPeakBytes(),before); EXPECT_LE(snapshot.allocationPeakBytes(),options.max_build_bytes);
    options.incoming=true; auto clone=snapshot.clone(options);
    EXPECT_GT(clone->allocationCurrentBytes(),snapshot.allocationCurrentBytes());
    EXPECT_LE(clone->allocationPeakBytes(),options.max_build_bytes);
    EXPECT_EQ(clone->extId(1),snapshot.extId(1));
}
