#include "TestTempDir.h"
#include "graph/Graphstorage.h"
#include "graph/Livegraph.h"
#include "graph/StorageCodec.h"
#include <gtest/gtest.h>
#include <fstream>
#include <cstring>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace nexora::graph;
namespace codec=storage_codec;
std::string contents(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary); return {std::istreambuf_iterator<char>(in),{}};
}
void corrupt(const std::filesystem::path& path,size_t offset) {
    std::fstream file(path,std::ios::binary|std::ios::in|std::ios::out);
    file.seekg(offset); char byte{}; file.read(&byte,1); byte^=0x40;
    file.seekp(offset); file.write(&byte,1);
}
void populate(LiveGraph& graph) {
    ASSERT_EQ(graph.addNode("alpha","User"),0U);
    ASSERT_EQ(graph.addNode("deleted","Discarded"),1U);
    ASSERT_EQ(graph.addNode("beta","User"),2U);
    ASSERT_NE(graph.addEdge("alpha","beta","KNOWS",false),kInvalidEdgeId);
    ASSERT_TRUE(graph.removeNode("deleted"));
}
void expectTraversal(LiveGraph& graph) {
    EXPECT_EQ(graph.activeNodeCount(),2U); EXPECT_EQ(graph.activeEdgeCount(),1U);
    EXPECT_TRUE(graph.hasNode("alpha")); EXPECT_TRUE(graph.hasNode("beta"));
    EXPECT_FALSE(graph.hasNode("deleted"));
    EXPECT_EQ(graph.neighborsExt("alpha",Direction::Out,"KNOWS"),(std::vector<ExtId>{"beta"}));
    EXPECT_EQ(graph.neighborsExt("beta",Direction::Out,"KNOWS"),(std::vector<ExtId>{"alpha"}));
    EXPECT_EQ(graph.getExtId(graph.getDenseId("beta")),"beta");
    EXPECT_EQ(graph.getNode(graph.getDenseId("alpha"))->type_name,"User");
    EXPECT_EQ(graph.getNode(graph.getDenseId("alpha"))->out_degree,1U);
    EXPECT_EQ(graph.getNode(graph.getDenseId("beta"))->out_degree,1U);
}

TEST(GraphStorageV2, HeaderAndRecordsHaveExplicitLittleEndianFieldsAndChecksums) {
    TestTempDir temp("storage_v2_encoding"); GraphStorage storage(temp.path(),"social");
    ASSERT_TRUE(storage.open()) << storage.lastError();
    NodeRecord node{}; node.dense_id=0; node.type_id=0x11223344; node.flags=FLAG_ACTIVE;
    node.out_degree=0x0102030405060708ULL; node.in_degree=UINT64_MAX;
    ASSERT_TRUE(storage.writeNode(node));
    auto raw=contents(storage.nodePath()); ASSERT_EQ(raw.size(),72U);
    EXPECT_EQ(raw.substr(0,4),"NXG2"); EXPECT_EQ(codec::get(raw,4,4),2U);
    EXPECT_EQ(codec::get(raw,8,4),1U); EXPECT_EQ(codec::get(raw,12,4),40U);
    EXPECT_EQ(codec::get(raw,16,4),1U); EXPECT_EQ(codec::get(raw,20,8),storage.generation());
    EXPECT_EQ(static_cast<unsigned char>(raw[40]),0x44U);
    EXPECT_EQ(static_cast<unsigned char>(raw[48]),0x08U);
    EXPECT_EQ(static_cast<unsigned char>(raw[55]),0x01U);
    auto loaded=storage.readNode(0); ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->out_degree,node.out_degree); EXPECT_EQ(loaded->in_degree,UINT64_MAX);
    GraphMeta meta{}; meta.kind=GraphKind::Static; meta.state=GraphState::Clean;
    meta.active_nodes=UINT64_MAX; meta.created_at=INT64_MIN; meta.updated_at=INT64_MAX;
    std::strcpy(meta.graph_name,"social"); std::strcpy(meta.filter_expr,"quote=\" braces={} backslash=\\");
    ASSERT_TRUE(storage.writeMeta(meta)); const auto metadata_path=storage.metaPath();
    storage.close(); ASSERT_TRUE(storage.open()) << storage.lastError();
    auto restored=storage.readMeta(); ASSERT_TRUE(restored);
    EXPECT_EQ(uint64_t(restored->active_nodes),UINT64_MAX);
    EXPECT_EQ(int64_t(restored->created_at),INT64_MIN); EXPECT_EQ(int64_t(restored->updated_at),INT64_MAX);
    EXPECT_STREQ(restored->filter_expr,meta.filter_expr);
    EXPECT_EQ(contents(metadata_path).substr(0,4),"NXG2");
}

TEST(GraphStorageV2, RejectsCorruptionInHeadersRecordsMetadataAndManifest) {
    for (int file_kind=0;file_kind<4;++file_kind) {
        for (size_t offset:{size_t{0},size_t{4},size_t{12},size_t{16},size_t{28},size_t{40}}) {
            if (file_kind==3 && offset>=32) continue;
            TestTempDir temp("storage_v2_corrupt"); GraphStorage storage(temp.path(),"social");
            ASSERT_TRUE(storage.open()); LiveGraph graph(&storage,nullptr,"social"); populate(graph);
            const auto path=file_kind==0?storage.nodePath():file_kind==1?storage.edgePath():
                            file_kind==2?storage.metaPath():storage.manifestPath();
            storage.close(); corrupt(path,offset); const auto damaged=contents(path);
            EXPECT_FALSE(storage.open()) << "kind=" << file_kind << " offset=" << offset;
            EXPECT_FALSE(storage.lastError().empty()); EXPECT_EQ(contents(path),damaged);
        }
    }
}

TEST(GraphStorageV2, RejectsTruncatedOrMissingReferencedFilesWithoutBootstrappingEmptyGraph) {
    for (int damage=0;damage<5;++damage) {
        TestTempDir temp("storage_v2_missing"); GraphStorage storage(temp.path(),"social");
        ASSERT_TRUE(storage.open()); LiveGraph graph(&storage,nullptr,"social"); populate(graph);
        const auto nodes=storage.nodePath(),edges=storage.edgePath(),manifest=storage.manifestPath();
        storage.close();
        if (damage==0) std::filesystem::resize_file(nodes,31);
        else if (damage==1) std::filesystem::resize_file(edges,71);
        else if (damage==2) std::filesystem::remove(nodes);
        else if (damage==3) std::filesystem::remove(manifest);
        else std::filesystem::resize_file(storage.metaPath(),40);
        EXPECT_FALSE(storage.open()); EXPECT_FALSE(storage.lastError().empty());
    }
}

TEST(GraphStorageV2, RuntimeScanReportsChecksumFailureInsteadOfReturningPartialSuccess) {
    TestTempDir temp("storage_v2_scan"); GraphStorage storage(temp.path(),"social");
    ASSERT_TRUE(storage.open()); LiveGraph graph(&storage,nullptr,"social"); populate(graph);
    corrupt(storage.nodePath(),32+40*2+8);
    EXPECT_THROW(storage.scanAllNodes([](const NodeRecord&) { return true; }),std::runtime_error);
    EXPECT_FALSE(storage.lastError().empty());
}

TEST(GraphStorageV2, ValidatesSlotIdsAndActiveEdgeEndpointsEvenWithValidChecksums) {
    for (bool bad_slot:{false,true}) {
        TestTempDir temp("storage_v2_structure"); GraphStorage storage(temp.path(),"social");
        ASSERT_TRUE(storage.open());
        NodeRecord node{}; node.dense_id=0; node.flags=FLAG_ACTIVE; ASSERT_TRUE(storage.writeNode(node));
        EdgeRecord edge{}; edge.edge_id=0; edge.src=0; edge.dst=bad_slot?0:999;
        edge.flags=FLAG_ACTIVE; ASSERT_TRUE(storage.writeEdge(edge));
        const auto path=storage.edgePath(); storage.close();
        if (bad_slot) {
            auto raw=contents(path); codec::put(raw,32,42,8);
            auto record=raw.substr(32); codec::seal(record,32); raw.replace(32,40,record);
            std::ofstream file(path,std::ios::binary|std::ios::trunc); file.write(raw.data(),raw.size());
        }
        EXPECT_FALSE(storage.open()); EXPECT_FALSE(storage.lastError().empty());
    }
}

TEST(GraphStorageV2, RejectsLegacyRawFilesAndSecondWriter) {
    TestTempDir temp("storage_v2_legacy");
    { std::ofstream file(temp.path()/"social.nex",std::ios::binary); file<<std::string(32,'\0'); }
    GraphStorage legacy(temp.path(),"social"); EXPECT_FALSE(legacy.open());
    EXPECT_NE(legacy.lastError().find("legacy"),std::string::npos);
    TestTempDir fresh("storage_v2_owner"); GraphStorage first(fresh.path(),"social"),second(fresh.path(),"social");
    ASSERT_TRUE(first.open()); EXPECT_FALSE(second.open()); first.close(); ASSERT_TRUE(second.open());
}

TEST(GraphCompactionV2, RemapsEndpointsIdentitiesAndTypesAndPreservesOldGeneration) {
    TestTempDir temp("compact_v2_reload"); const auto wal_path=temp.path()/"graph.wal";
    std::filesystem::path old_nodes,old_edges,old_meta; std::string nodes,edges,meta;
    {
        GraphStorage storage(temp.path(),"social"); GraphWAL wal(wal_path);
        ASSERT_TRUE(storage.open()); ASSERT_TRUE(wal.open()); LiveGraph graph(&storage,&wal,"social"); populate(graph);
        old_nodes=storage.nodePath(); old_edges=storage.edgePath(); old_meta=storage.metaPath();
        nodes=contents(old_nodes); edges=contents(old_edges); meta=contents(old_meta);
        ASSERT_TRUE(graph.compact()); EXPECT_EQ(storage.generation(),2U); expectTraversal(graph);
        EXPECT_EQ(graph.getDenseId("beta"),1U);
        auto edge=storage.readEdge(0); ASSERT_TRUE(edge); EXPECT_EQ(edge->src,0U); EXPECT_EQ(edge->dst,1U);
        EXPECT_EQ(contents(old_nodes),nodes); EXPECT_EQ(contents(old_edges),edges); EXPECT_EQ(contents(old_meta),meta);
    }
    GraphStorage storage(temp.path(),"social"); GraphWAL wal(wal_path);
    ASSERT_TRUE(storage.open()) << storage.lastError(); ASSERT_TRUE(wal.open());
    LiveGraph recovered(&storage,&wal,"social"); ASSERT_TRUE(recovered.loadFromDisk());
    EXPECT_EQ(recovered.replayWAL(),0U); expectTraversal(recovered);
    ASSERT_NE(recovered.addNode("gamma","Other"),kInvalidDenseId);
    ASSERT_NE(recovered.addEdge("beta","gamma","NEXT",true),kInvalidEdgeId);
    EXPECT_EQ(recovered.neighborsExt("beta",Direction::Out,"NEXT"),(std::vector<ExtId>{"gamma"}));
}

TEST(GraphCompactionV2, KillAtEveryPublicationBoundarySelectsCompleteOldOrNewGeneration) {
    const std::vector<CompactionPoint> points={CompactionPoint::NodesStaged,CompactionPoint::EdgesStaged,
        CompactionPoint::MetadataStaged,CompactionPoint::BeforeManifest,CompactionPoint::ManifestPublished,
        CompactionPoint::Activated};
    for (const auto point:points) {
        TestTempDir temp("compact_v2_kill"); const auto wal_path=temp.path()/"graph.wal";
        {
            GraphStorage storage(temp.path(),"social"); GraphWAL wal(wal_path);
            ASSERT_TRUE(storage.open()); ASSERT_TRUE(wal.open()); LiveGraph graph(&storage,&wal,"social"); populate(graph);
        }
        int notify[2]; ASSERT_EQ(::pipe(notify),0); const auto pid=::fork(); ASSERT_GE(pid,0);
        if (pid==0) {
            ::close(notify[0]); GraphStorage storage(temp.path(),"social"); GraphWAL wal(wal_path);
            if (!storage.open() || !wal.open()) ::_exit(2);
            LiveGraph graph(&storage,&wal,"social"); if (!graph.loadFromDisk()) ::_exit(3);
            storage.setCompactionHookForTesting([&](CompactionPoint current) {
                if (current==point) { const char ready='R'; if (::write(notify[1],&ready,1)!=1) ::_exit(4); for (;;) ::pause(); }
            });
            (void)graph.compact(); ::_exit(5);
        }
        ::close(notify[1]); char ready{}; const auto received=::read(notify[0],&ready,1); ::close(notify[0]);
        ::kill(pid,SIGKILL); int status=0; ASSERT_EQ(::waitpid(pid,&status,0),pid);
        ASSERT_EQ(received,1); ASSERT_EQ(ready,'R'); ASSERT_TRUE(WIFSIGNALED(status));
        GraphStorage storage(temp.path(),"social"); GraphWAL wal(wal_path);
        ASSERT_TRUE(storage.open()) << storage.lastError(); ASSERT_TRUE(wal.open());
        LiveGraph graph(&storage,&wal,"social"); ASSERT_TRUE(graph.loadFromDisk());
        EXPECT_EQ(graph.replayWAL(),0U); expectTraversal(graph);
        const bool committed=point==CompactionPoint::ManifestPublished || point==CompactionPoint::Activated;
        EXPECT_EQ(storage.generation(),committed?2U:1U);
        EXPECT_EQ(graph.getDenseId("beta"),committed?1U:2U);
        // Orphan staging files do not prevent a later successful compaction.
        ASSERT_TRUE(graph.compact()); expectTraversal(graph);
    }
}

TEST(GraphCompactionV2, InjectedPublicationFailurePropagatesAndReopenUsesOldGeneration) {
    TestTempDir temp("compact_v2_failure"); GraphStorage storage(temp.path(),"social");
    ASSERT_TRUE(storage.open()); LiveGraph graph(&storage,nullptr,"social"); populate(graph);
    storage.setCompactionHookForTesting([](CompactionPoint point) {
        if (point==CompactionPoint::BeforeManifest) throw std::runtime_error("injected disk failure");
    });
    EXPECT_THROW(graph.compact(),std::runtime_error); EXPECT_FALSE(storage.lastError().empty());
    storage.close(); ASSERT_TRUE(storage.open()); EXPECT_EQ(storage.generation(),1U);
    ASSERT_TRUE(graph.loadFromDisk()); expectTraversal(graph);
}

TEST(GraphCompactionV2, CompatibilityPairDoesNotPublishNodesBeforeEdgesAreReady) {
    TestTempDir temp("compact_v2_pair"); GraphStorage storage(temp.path(),"social");
    ASSERT_TRUE(storage.open()); LiveGraph graph(&storage,nullptr,"social"); populate(graph);
    const auto original=contents(storage.manifestPath());
    std::unordered_map<DenseId,DenseId> nodes; std::unordered_map<EdgeId,EdgeId> edges;
    ASSERT_TRUE(storage.compactNodes(nodes)); EXPECT_EQ(contents(storage.manifestPath()),original);
    EXPECT_EQ(storage.nodeFileSize(),3U); ASSERT_TRUE(storage.compactEdges(nodes,edges));
    EXPECT_EQ(storage.nodeFileSize(),2U); ASSERT_TRUE(graph.loadFromDisk()); expectTraversal(graph);
}

TEST(GraphCompactionV2, AsyncWalCheckpointFencesOldIdsAcrossCompactionAndRestart) {
    TestTempDir temp("compact_v2_async"); const auto wal_path=temp.path()/"graph.wal";
    WalOptions options; options.durability=WalDurability::Async;
    {
        GraphStorage storage(temp.path(),"social"); GraphWAL wal(wal_path,86400,options);
        ASSERT_TRUE(storage.open()); ASSERT_TRUE(wal.open()); LiveGraph graph(&storage,&wal,"social"); populate(graph);
        ASSERT_TRUE(graph.compact()); expectTraversal(graph);
    }
    GraphStorage storage(temp.path(),"social"); GraphWAL wal(wal_path,86400,options);
    ASSERT_TRUE(storage.open()); ASSERT_TRUE(wal.open()); LiveGraph graph(&storage,&wal,"social");
    ASSERT_TRUE(graph.loadFromDisk()); EXPECT_EQ(graph.replayWAL(),0U); expectTraversal(graph);
    EXPECT_EQ(graph.getDenseId("beta"),1U);
}

TEST(GraphStorageV2, SparseSlotsAndEscapedUnicodeIdentitiesRoundTrip) {
    TestTempDir temp("storage_v2_unicode"); GraphStorage storage(temp.path(),"social");
    const std::string ext="سلام-quote\"-backslash\\-comma,-brace{}";
    ASSERT_TRUE(storage.open()); ASSERT_TRUE(storage.rememberNode(3,ext,7,"کاربر"));
    NodeRecord node{}; node.dense_id=3; node.type_id=7; node.flags=FLAG_ACTIVE;
    ASSERT_TRUE(storage.writeNode(node)); storage.close();
    ASSERT_TRUE(storage.open()); LiveGraph graph(&storage,nullptr,"social"); ASSERT_TRUE(graph.loadFromDisk());
    EXPECT_EQ(graph.getDenseId(ext),3U); EXPECT_EQ(graph.getExtId(3),ext);
    EXPECT_EQ(graph.getNode(3)->type_name,"کاربر");
    EXPECT_EQ(graph.activeNodeCount(),1U);
}

TEST(GraphStorageV2, ImplicitNodePromotionPersistsIdentityAndExplicitType) {
    TestTempDir temp("storage_v2_promotion"); GraphStorage storage(temp.path(),"social");
    ASSERT_TRUE(storage.open());
    {
        LiveGraph graph(&storage,nullptr,"social");
        ASSERT_NE(graph.addEdge("a","b","KNOWS",true),kInvalidEdgeId);
        ASSERT_EQ(graph.addNode("a","User"),0U);
        EXPECT_EQ(graph.getNode(0)->type_name,"User");
    }
    storage.close(); ASSERT_TRUE(storage.open());
    LiveGraph graph(&storage,nullptr,"social"); ASSERT_TRUE(graph.loadFromDisk());
    EXPECT_EQ(graph.getDenseId("a"),0U); EXPECT_EQ(graph.getNode(0)->type_name,"User");
    EXPECT_EQ(graph.neighborsExt("a",Direction::Out,"KNOWS"),(std::vector<ExtId>{"b"}));
}
} // namespace
