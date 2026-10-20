#include "TestTempDir.h"
#include "core/DocEngine.h"
#include "graph/GraphManager.h"
#include <gtest/gtest.h>
#include <future>
#include <thread>
using namespace nexora::graph;
using namespace nexora::core;
using namespace std::chrono_literals;

TEST(ThreadingContract, MetadataCacheInvalidatesSchemaForeignKeyAndIndex) {
    TestTempDir temp("metadata_cache"); DocEngine engine(temp.path().string());
    ASSERT_TRUE(engine.CreateCollection("parents").success);
    ASSERT_TRUE(engine.CreateCollection("items").success);
    ASSERT_TRUE(engine.GetSchema("items").has_value());
    EXPECT_TRUE(engine.GetForeignKeys("items").empty());
    EXPECT_TRUE(engine.GetIndexes("items").empty());
    SchemaDefinition schema; schema.strict=true;
    ASSERT_TRUE(engine.SetSchemaValidation("items",schema).success);
    EXPECT_TRUE(engine.GetSchema("items")->strict);
    ForeignKeyDefinition fk{"parent_fk","parent","parents","_id"};
    ASSERT_TRUE(engine.AddForeignKey("items",fk).success);
    ASSERT_EQ(engine.GetForeignKeys("items").size(),1U);
    ASSERT_TRUE(engine.DropForeignKey("items","parent_fk").success);
    EXPECT_TRUE(engine.GetForeignKeys("items").empty());
    IndexDefinition index; index.index_name="by_value"; index.fields={"value"};
    ASSERT_TRUE(engine.CreateIndex("items",index).success);
    ASSERT_EQ(engine.GetIndexes("items").size(),1U);
    EXPECT_EQ(engine.GetIndexes("items")[0].state,IndexState::Ready);
    ASSERT_TRUE(engine.DropIndex("items","by_value").success);
    EXPECT_TRUE(engine.GetIndexes("items").empty());
    ASSERT_TRUE(engine.DropCollection("items").success);
    EXPECT_FALSE(engine.GetSchema("items").has_value());
    ASSERT_TRUE(engine.CreateCollection("items").success);
    EXPECT_FALSE(engine.GetSchema("items")->strict);
}

TEST(ThreadingContract, ConcurrentMetadataReadersDdlAndIndependentCollections) {
    TestTempDir temp("metadata_threads"); DocEngine engine(temp.path().string());
    ASSERT_TRUE(engine.CreateCollection("metadata").success);
    for (int i=0;i<4;++i) ASSERT_TRUE(engine.CreateCollection("c"+std::to_string(i)).success);
    std::atomic<bool> stop=false; std::atomic<int> failures=0;
    std::thread reader([&] {
        while (!stop.load()) {
            if (!engine.GetSchema("metadata")) ++failures;
            (void)engine.GetForeignKeys("metadata"); (void)engine.GetIndexes("metadata");
        }
    });
    std::vector<std::thread> writers;
    for (int i=0;i<4;++i) writers.emplace_back([&,i] {
        for (int j=0;j<100;++j)
            if (!engine.InsertOne("c"+std::to_string(i),"{\"value\":1}").success) ++failures;
    });
    for (int i=0;i<30;++i) {
        SchemaDefinition schema; schema.strict=bool(i%2);
        if (!engine.SetSchemaValidation("metadata",schema).success) ++failures;
    }
    for (auto& writer:writers) writer.join();
    stop=true; reader.join();
    EXPECT_EQ(failures,0);
    for (int i=0;i<4;++i) EXPECT_EQ(engine.GetCollectionSize("c"+std::to_string(i)),100);
}

namespace {
class PausedAlgorithm final:public algorithms::LockAlgorithm {
public:
    std::promise<void> entered; std::shared_future<void> resume;
    explicit PausedAlgorithm(std::shared_future<void> signal):resume(std::move(signal)) {}
    std::string name() const override { return "paused"; }
    algorithms::AlgoResult run(const LiveGraph&,const std::vector<ExtId>&) override {
        entered.set_value(); resume.wait(); return {true,"","",0};
    }
};
}
TEST(ThreadingContract, SlowAlgorithmDoesNotHoldRegistryAndDropIsBusy) {
    TestTempDir temp("graph_registry"); DocEngine engine((temp.path()/"db").string());
    GraphManager manager(&engine,temp.path()/"graphs"); GraphDefinition def; def.name="first";
    ASSERT_TRUE(manager.createGraph(def)); ASSERT_TRUE(manager.buildGraph("first").success);
    std::promise<void> resume; PausedAlgorithm algorithm(resume.get_future().share());
    auto entered=algorithm.entered.get_future();
    auto job=std::async(std::launch::async,[&] { return manager.runLock("first",algorithm,{}); });
    const auto entered_status=entered.wait_for(2s);
    if (entered_status!=std::future_status::ready) { resume.set_value(); FAIL()<<"algorithm did not start"; }
    auto registry=std::async(std::launch::async,[&] {
        auto names=manager.listGraphs(); GraphDefinition other; other.name="second";
        return names.size()==1 && manager.createGraph(other);
    });
    const auto status=registry.wait_for(2s);
    // Release before assertions so a regression never strands the paused job.
    resume.set_value(); EXPECT_EQ(status,std::future_status::ready);
    EXPECT_TRUE(registry.get()); EXPECT_TRUE(job.get().success);
    {
        auto lease=manager.shareLiveGraph("first"); ASSERT_TRUE(lease);
        EXPECT_FALSE(manager.dropGraph("first")); EXPECT_EQ(lease->activeNodeCount(),0U);
    }
    EXPECT_TRUE(manager.dropGraph("first"));
}

TEST(ThreadingContract, ConcurrentSnapshotCacheAndReadLeaseLifetime) {
    TestTempDir temp("snapshot_threads"); DocEngine engine((temp.path()/"db").string());
    GraphManager manager(&engine,temp.path()/"graphs");
    GraphDefinition def; def.name="graph"; ASSERT_TRUE(manager.createGraph(def));
    auto graph=manager.shareLiveGraph("graph"); graph->addEdge("a","b","link"); graph.reset();
    std::vector<std::future<size_t>> jobs;
    for (int i=0;i<8;++i) jobs.push_back(std::async(std::launch::async,[&] {
        SnapshotOptions options; options.use_cache=true;
        auto snapshot=manager.createSnapshot("graph",{},{},options);
        return snapshot->edgeCount();
    }));
    for (auto& job:jobs) EXPECT_EQ(job.get(),1U);
    {
        auto [pointer,lease]=manager.acquireReadLock("graph"); ASSERT_TRUE(pointer);
        EXPECT_FALSE(manager.dropGraph("graph")); EXPECT_EQ(pointer->activeEdgeCount(),1U);
    }
    EXPECT_TRUE(manager.dropGraph("graph"));
}

TEST(ThreadingContract, WriterProgressUnderContinuousReaders) {
    LiveGraph graph(nullptr,nullptr,"writer_progress"); graph.addNode("root","Node");
    std::atomic<bool> stop=false; std::vector<std::thread> readers;
    for (int i=0;i<8;++i) readers.emplace_back([&] { while (!stop.load()) (void)graph.stats(); });
    auto writer=std::async(std::launch::async,[&] { return graph.addNode("writer","Node"); });
    const auto status=writer.wait_for(2s); stop=true;
    for (auto& reader:readers) reader.join();
    EXPECT_EQ(status,std::future_status::ready); (void)writer.get(); EXPECT_TRUE(graph.hasNode("writer"));
}

TEST(ThreadingContract, PageBytesCloseAndSnapshotEdgeRows) {
    TestTempDir temp("bounded_pages"); DocEngine engine((temp.path()/"db").string());
    ASSERT_TRUE(engine.CreateCollection("items").success);
    for (int i=0;i<20;++i) ASSERT_TRUE(engine.InsertOne("items","{\"_id\":\"n"+std::to_string(i)+"\",\"value\":123}").success);
    auto first=engine.FindPage("items",{},20,"",100);
    ASSERT_TRUE(first.success); EXPECT_LE(first.data.size(),100U); ASSERT_FALSE(first.continuation_token.empty());
    engine.ClosePage(first.continuation_token);
    EXPECT_FALSE(engine.FindPage("items",{},20,first.continuation_token,100).success);
    std::string cursor; size_t pages=0;
    do {
        const auto page=engine.FindPage("items",{},20,cursor,100);
        ASSERT_TRUE(page.success); EXPECT_LE(page.data.size(),100U);
        cursor=page.continuation_token; ++pages; ASSERT_LT(pages,30U);
    } while (!cursor.empty());
    EXPECT_GT(pages,1U); EXPECT_FALSE(engine.FindPage("items",{},20,"",2).success);
    LiveGraph graph(nullptr,nullptr,"edges"); graph.addEdge("a","b","link",false);
    StaticGraph snapshot(graph); const auto edge=snapshot.edgeAt(0);
    ASSERT_TRUE(edge); EXPECT_EQ((*edge)[4],0U); EXPECT_FALSE(snapshot.edgeAt(1));
}
