#include "TestTempDir.h"
#include "graph/Graphwal.h"
#include "graph/Graphstorage.h"
#include "graph/Livegraph.h"
#include <gtest/gtest.h>
#include <atomic>
#include <barrier>
#include <fstream>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

namespace {
using namespace nexora::graph;
std::string bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
void flip(const std::filesystem::path& path, size_t offset) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(offset); char c{}; file.read(&c,1); c ^= 0x40;
    file.seekp(offset); file.write(&c,1);
}

TEST(GraphWalV2, ImmutableChecksummedRecordAndSeparateContiguousCheckpoint) {
    TestTempDir temp("wal_v2_checkpoint"); const auto path = temp.path()/"graph.wal";
    GraphWAL wal(path); ASSERT_TRUE(wal.open()) << wal.lastError();
    const auto a = wal.append(WalOpType::AddNode,42,0,0,7,FLAG_ACTIVE);
    const auto original = bytes(path);
    ASSERT_EQ(original.size(),GraphWAL::kRecordBytes);
    EXPECT_EQ(original.substr(0,4),"NXW2");
    const auto b = wal.append(WalOpType::RemoveNode,42);
    ASSERT_TRUE(wal.markApplied(b)); EXPECT_EQ(wal.appliedLsn(),0U);
    ASSERT_TRUE(wal.markApplied(a)); EXPECT_EQ(wal.appliedLsn(),b);
    EXPECT_EQ(bytes(path).substr(0,original.size()),original);
    EXPECT_EQ(bytes(path.string()+".checkpoint").substr(0,4),"NXC2");
    wal.close(); ASSERT_TRUE(wal.open()) << wal.lastError();
    EXPECT_EQ(wal.appliedLsn(),b); EXPECT_TRUE(wal.loadUnapplied().empty());
    EXPECT_EQ(wal.append(WalOpType::AddNode,99),b+1);
}

TEST(GraphWalV2, RepairsEveryIncompleteTailLengthWithoutLosingCompleteRecord) {
    for (size_t tail=1; tail<GraphWAL::kRecordBytes; ++tail) {
        TestTempDir temp("wal_v2_tail"); const auto path=temp.path()/"graph.wal";
        GraphWAL wal(path); ASSERT_TRUE(wal.open());
        ASSERT_EQ(wal.append(WalOpType::AddNode,1),1U); wal.close();
        { std::ofstream file(path,std::ios::binary|std::ios::app); file << std::string(tail,'x'); }
        ASSERT_TRUE(wal.open()) << "tail=" << tail << ' ' << wal.lastError();
        EXPECT_EQ(std::filesystem::file_size(path),GraphWAL::kRecordBytes);
        EXPECT_EQ(wal.loadUnapplied().size(),1U);
        EXPECT_EQ(wal.append(WalOpType::AddNode,2),2U);
    }
}

TEST(GraphWalV2, RejectsRecordAndCheckpointCorruptionWithoutTruncation) {
    for (const size_t offset : {size_t{0},size_t{4},size_t{8},size_t{12},size_t{40},size_t{71}}) {
        TestTempDir temp("wal_v2_corrupt"); const auto path=temp.path()/"graph.wal";
        GraphWAL wal(path); ASSERT_TRUE(wal.open());
        ASSERT_NE(wal.append(WalOpType::AddNode,1),UINT64_MAX); wal.close();
        flip(path,offset); const auto corrupt=bytes(path);
        EXPECT_FALSE(wal.open()); EXPECT_FALSE(wal.lastError().empty());
        EXPECT_EQ(bytes(path),corrupt);
    }
    TestTempDir temp("wal_v2_cp_corrupt"); const auto path=temp.path()/"graph.wal";
    GraphWAL wal(path); ASSERT_TRUE(wal.open());
    ASSERT_TRUE(wal.markApplied(wal.append(WalOpType::AddNode,1))); wal.close();
    flip(path.string()+".checkpoint",8); EXPECT_FALSE(wal.open());
}

TEST(GraphWalV2, RepairsIncompleteFirstRecordAtEveryByteBoundary) {
    for (size_t tail=1; tail<GraphWAL::kRecordBytes; ++tail) {
        TestTempDir temp("wal_v2_first_tail"); const auto path=temp.path()/"graph.wal";
        GraphWAL wal(path); ASSERT_TRUE(wal.open());
        ASSERT_EQ(wal.append(WalOpType::AddNode,1),1U); wal.close();
        std::filesystem::resize_file(path,tail);
        ASSERT_TRUE(wal.open()) << "tail=" << tail << ' ' << wal.lastError();
        EXPECT_EQ(std::filesystem::file_size(path),0U);
        EXPECT_TRUE(wal.loadUnapplied().empty());
        EXPECT_EQ(wal.append(WalOpType::AddNode,2),1U);
    }
}

TEST(GraphWalV2, SegmentRetentionNeverRewritesRetainedBytesAndNeverDropsPending) {
    TestTempDir temp("wal_v2_segments"); const auto path=temp.path()/"graph.wal";
    WalOptions options; options.segment_bytes=GraphWAL::kRecordBytes;
    GraphWAL wal(path,-1,options); ASSERT_TRUE(wal.open());
    const auto a=wal.append(WalOpType::AddNode,1);
    const auto b=wal.append(WalOpType::AddNode,2);
    const auto c=wal.append(WalOpType::AddNode,3);
    const auto active=path.string()+".segment.00000000000000000003";
    const auto retained=bytes(active);
    ASSERT_TRUE(wal.markApplied(b)); EXPECT_EQ(wal.purgeOld(),0U);
    ASSERT_TRUE(wal.markApplied(a)); EXPECT_EQ(wal.purgeOld(),2U);
    EXPECT_EQ(bytes(active),retained); EXPECT_FALSE(std::filesystem::exists(path));
    wal.close(); ASSERT_TRUE(wal.open()) << wal.lastError();
    ASSERT_EQ(wal.loadUnapplied().size(),1U);
    EXPECT_EQ(wal.loadUnapplied().front().seq,c);
    EXPECT_EQ(wal.append(WalOpType::AddNode,4),c+1);
}

TEST(GraphWalV2, RejectsSecondWriterAndUnavailablePath) {
    TestTempDir temp("wal_v2_lock"); const auto path=temp.path()/"graph.wal";
    GraphWAL first(path),second(path); ASSERT_TRUE(first.open());
    EXPECT_FALSE(second.open()); first.close(); EXPECT_TRUE(second.open());
    GraphWAL invalid(temp.path()/"missing"/"graph.wal"); EXPECT_FALSE(invalid.open());
    EXPECT_EQ(invalid.append(WalOpType::AddNode,1),UINT64_MAX);
}

TEST(GraphWalV2, AsyncDefersSyncAndConservativelyReplaysVolatileAppliedMarkers) {
    TestTempDir temp("wal_v2_async"); const auto path=temp.path()/"graph.wal";
    WalOptions options; options.durability=WalDurability::Async;
    GraphWAL wal(path,-1,options); ASSERT_TRUE(wal.open());
    const auto before=wal.syncCount(); const auto a=wal.append(WalOpType::AddNode,1);
    ASSERT_TRUE(wal.markApplied(a)); EXPECT_EQ(wal.syncCount(),before);
    EXPECT_FALSE(std::filesystem::exists(path.string()+".checkpoint"));
    EXPECT_EQ(wal.purgeOld(),0U); ASSERT_TRUE(wal.flush());
    EXPECT_GT(wal.syncCount(),before); wal.close(); ASSERT_TRUE(wal.open());
    EXPECT_EQ(wal.loadUnapplied().size(),1U);
}

TEST(GraphWalV2, GroupCommitSharesDurabilityBarrierBetweenConcurrentAppenders) {
    TestTempDir temp("wal_v2_group"); WalOptions options;
    options.durability=WalDurability::GroupCommit; options.group_delay=std::chrono::milliseconds(25);
    GraphWAL wal(temp.path()/"graph.wal",86400,options); ASSERT_TRUE(wal.open());
    constexpr size_t count=12; std::barrier ready(static_cast<std::ptrdiff_t>(count));
    std::vector<std::thread> writers; std::atomic<size_t> success{0};
    const auto before=wal.syncCount();
    for (size_t i=0;i<count;++i) writers.emplace_back([&,i] {
        ready.arrive_and_wait(); if (wal.append(WalOpType::AddNode,i)!=UINT64_MAX) ++success;
    });
    for (auto& writer:writers) writer.join();
    EXPECT_EQ(success,count); EXPECT_LT(wal.syncCount()-before,count);
    EXPECT_EQ(wal.totalEntries(),count);
}

TEST(GraphWalV2, SyncAppendSurvivesProcessExitWithoutDestructors) {
    TestTempDir temp("wal_v2_process_exit"); const auto path=temp.path()/"graph.wal";
    const auto pid=::fork(); ASSERT_GE(pid,0);
    if (pid==0) {
        GraphWAL wal(path);
        if (!wal.open() || wal.append(WalOpType::AddNode,7)==UINT64_MAX) ::_exit(2);
        ::_exit(0);
    }
    int status=0; ASSERT_EQ(::waitpid(pid,&status,0),pid); ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status),0);
    GraphWAL recovered(path); ASSERT_TRUE(recovered.open()) << recovered.lastError();
    ASSERT_EQ(recovered.loadUnapplied().size(),1U);
    EXPECT_EQ(recovered.loadUnapplied().front().node_or_edge_id,7U);
}

TEST(GraphWalV2, ReplayAlreadyPersistedEdgesIsIdempotentAcrossRepeatedRestarts) {
    TestTempDir temp("wal_v2_redo"); const auto path=temp.path()/"graph.wal";
    WalOptions options; options.durability=WalDurability::Async;
    for (int restart=0;restart<3;++restart) {
        GraphStorage storage(temp.path(),"social"); ASSERT_TRUE(storage.open());
        GraphWAL wal(path,86400,options); ASSERT_TRUE(wal.open());
        LiveGraph graph(&storage,&wal,"social");
        if (restart==0) {
            ASSERT_NE(graph.addNode("a","User"),kInvalidDenseId);
            ASSERT_NE(graph.addNode("b","User"),kInvalidDenseId);
            ASSERT_NE(graph.addEdge("a","b","follows",false),kInvalidEdgeId);
        } else {
            ASSERT_TRUE(graph.loadFromDisk()); ASSERT_EQ(graph.replayWAL(),3U);
            EXPECT_EQ(graph.activeNodeCount(),2U); EXPECT_EQ(graph.activeEdgeCount(),1U);
            EXPECT_EQ(graph.neighbors(0,Direction::Out).size(),1U);
            EXPECT_EQ(graph.neighbors(1,Direction::Out).size(),1U);
            EXPECT_EQ(graph.replayWAL(),0U);
        }
    }
}

TEST(GraphWalV2, KillBetweenLogDataAndCheckpointRecoversSameGraph) {
    for (int boundary=0;boundary<3;++boundary) {
        TestTempDir temp("wal_v2_kill_boundaries"); const auto path=temp.path()/"graph.wal";
        int notify[2]; ASSERT_EQ(::pipe(notify),0);
        const auto pid=::fork(); ASSERT_GE(pid,0);
        if (pid==0) {
            ::close(notify[0]);
            GraphStorage storage(temp.path(),"social"); GraphWAL wal(path);
            if (!storage.open() || !wal.open()) ::_exit(2);
            const auto seq=wal.append(WalOpType::AddNode,0,0,0,1,FLAG_ACTIVE);
            if (seq==UINT64_MAX) ::_exit(3);
            if (boundary>=1) {
                NodeRecord node{}; node.dense_id=0; node.type_id=1; node.flags=FLAG_ACTIVE;
                if (!storage.writeNode(node) || !storage.durableSync()) ::_exit(4);
            }
            if (boundary>=2 && !wal.markApplied(seq)) ::_exit(5);
            const char done='R'; if (::write(notify[1],&done,1)!=1) ::_exit(6);
            for (;;) ::pause();
        }
        ::close(notify[1]); char ready{};
        const auto received=::read(notify[0],&ready,1); ::close(notify[0]);
        // Always reap the child, even if setup failed.
        ::kill(pid,SIGKILL); int status=0; ASSERT_EQ(::waitpid(pid,&status,0),pid);
        ASSERT_EQ(received,1); ASSERT_EQ(ready,'R'); ASSERT_TRUE(WIFSIGNALED(status));
        GraphStorage storage(temp.path(),"social"); GraphWAL wal(path);
        ASSERT_TRUE(storage.open()); ASSERT_TRUE(wal.open()) << wal.lastError();
        LiveGraph graph(&storage,&wal,"social"); ASSERT_TRUE(graph.loadFromDisk());
        EXPECT_EQ(graph.replayWAL(),boundary<2 ? 1U : 0U);
        EXPECT_EQ(graph.activeNodeCount(),1U); EXPECT_EQ(graph.replayWAL(),0U);
    }
}

TEST(GraphWalV2, EdgeSurvivesSigkillAtEveryDurabilityBoundaryWithoutDuplicates) {
    for (bool directed : {false,true}) for (int boundary=0;boundary<3;++boundary) {
        TestTempDir temp("wal_v2_edge_kill"); const auto path=temp.path()/"graph.wal";
        {
            GraphStorage storage(temp.path(),"social"); GraphWAL wal(path);
            ASSERT_TRUE(storage.open()); ASSERT_TRUE(wal.open());
            LiveGraph graph(&storage,&wal,"social");
            ASSERT_EQ(graph.addNode("a","User"),0U);
            ASSERT_EQ(graph.addNode("b","User"),1U);
            ASSERT_TRUE(storage.rememberEdgeType(0,"knows"));
        }
        int notify[2]; ASSERT_EQ(::pipe(notify),0);
        const auto pid=::fork(); ASSERT_GE(pid,0);
        if (pid==0) {
            ::close(notify[0]);
            GraphStorage storage(temp.path(),"social"); GraphWAL wal(path);
            if (!storage.open() || !wal.open()) ::_exit(2);
            const uint32_t flags=FLAG_ACTIVE | (directed ? 0U : uint32_t(FLAG_UNDIRECTED));
            const auto seq=wal.append(WalOpType::AddEdge,0,0,1,0,flags);
            if (seq==UINT64_MAX) ::_exit(3);
            if (boundary>=1) {
                EdgeRecord edge{}; edge.edge_id=0; edge.src=0; edge.dst=1; edge.flags=flags;
                auto src=storage.readNode(0), dst=storage.readNode(1);
                if (!src || !dst) ::_exit(4);
                src->out_degree=1; dst->in_degree=1;
                if (!directed) { src->in_degree=1; dst->out_degree=1; }
                if (!storage.writeEdge(edge) || !storage.writeNode(*src) ||
                    !storage.writeNode(*dst) || !storage.durableSync()) ::_exit(4);
            }
            if (boundary>=2 && !wal.markApplied(seq)) ::_exit(5);
            const char done='R'; if (::write(notify[1],&done,1)!=1) ::_exit(6);
            for (;;) ::pause();
        }
        ::close(notify[1]); char ready{};
        const auto received=::read(notify[0],&ready,1); ::close(notify[0]);
        ::kill(pid,SIGKILL); int status=0; ASSERT_EQ(::waitpid(pid,&status,0),pid);
        ASSERT_EQ(received,1); ASSERT_EQ(ready,'R'); ASSERT_TRUE(WIFSIGNALED(status));
        ASSERT_EQ(WTERMSIG(status),SIGKILL);
        for (int restart=0;restart<3;++restart) {
            GraphStorage storage(temp.path(),"social"); GraphWAL wal(path);
            ASSERT_TRUE(storage.open()) << storage.lastError();
            ASSERT_TRUE(wal.open()) << wal.lastError();
            LiveGraph graph(&storage,&wal,"social"); ASSERT_TRUE(graph.loadFromDisk());
            EXPECT_EQ(graph.replayWAL(),restart==0 && boundary<2 ? 1U : 0U);
            EXPECT_EQ(graph.activeNodeCount(),2U); EXPECT_EQ(graph.activeEdgeCount(),1U);
            EXPECT_EQ(graph.neighbors(0,Direction::Out).size(),1U);
            EXPECT_EQ(graph.neighbors(1,Direction::Out).size(),directed ? 0U : 1U);
            EXPECT_EQ(graph.neighbors(1,Direction::In).size(),1U);
            ASSERT_TRUE(storage.readNode(0)); ASSERT_TRUE(storage.readNode(1));
            EXPECT_EQ(storage.readNode(0)->out_degree,1U);
            EXPECT_EQ(storage.readNode(1)->in_degree,1U);
            EXPECT_EQ(storage.readNode(1)->out_degree,directed ? 0U : 1U);
            EXPECT_EQ(graph.replayWAL(),0U);
        }
    }
}

TEST(GraphWalV2, RepeatedRedoOfEdgeAndNodeDeletionDoesNotUnderflowOrResurrect) {
    for (bool delete_node : {false,true}) {
        TestTempDir temp("wal_v2_delete_redo"); const auto path=temp.path()/"graph.wal";
        WalOptions options; options.durability=WalDurability::Async;
        for (int restart=0;restart<3;++restart) {
            GraphStorage storage(temp.path(),"social"); GraphWAL wal(path,86400,options);
            ASSERT_TRUE(storage.open()); ASSERT_TRUE(wal.open());
            LiveGraph graph(&storage,&wal,"social");
            if (restart==0) {
                const auto edge=graph.addEdge("a","b","knows",false);
                ASSERT_NE(edge,kInvalidEdgeId);
                if (delete_node) ASSERT_TRUE(graph.removeNode("a"));
                else ASSERT_TRUE(graph.removeEdge(edge));
            } else {
                ASSERT_TRUE(graph.loadFromDisk()); EXPECT_EQ(graph.replayWAL(),4U);
                EXPECT_EQ(graph.activeNodeCount(),delete_node ? 1U : 2U);
                EXPECT_EQ(graph.activeEdgeCount(),0U);
                EXPECT_TRUE(graph.neighbors(0,Direction::Both).empty());
                EXPECT_TRUE(graph.neighbors(1,Direction::Both).empty());
                EXPECT_EQ(graph.replayWAL(),0U);
            }
        }
    }
}

TEST(GraphWalV2, ExplicitAsyncCheckpointUsesDataBarrierAndSurvivesReopen) {
    TestTempDir temp("wal_v2_explicit_cp"); const auto path=temp.path()/"graph.wal";
    WalOptions options; options.durability=WalDurability::Async;
    GraphWAL wal(path,86400,options); ASSERT_TRUE(wal.open());
    const auto seq=wal.append(WalOpType::AddNode,1); ASSERT_TRUE(wal.markApplied(seq));
    bool synced=false;
    ASSERT_TRUE(wal.checkpoint([&] { synced=true; return true; })); EXPECT_TRUE(synced);
    wal.close(); ASSERT_TRUE(wal.open()); EXPECT_TRUE(wal.loadUnapplied().empty());
    const auto next=wal.append(WalOpType::AddNode,2); ASSERT_TRUE(wal.markApplied(next));
    EXPECT_FALSE(wal.checkpoint([] { return false; }));
    EXPECT_EQ(wal.append(WalOpType::AddNode,3),UINT64_MAX);
    wal.close(); ASSERT_TRUE(wal.open()); ASSERT_EQ(wal.loadUnapplied().size(),1U);
    EXPECT_EQ(wal.loadUnapplied().front().seq,next);
}

TEST(GraphWalV2, FailedCheckpointDoesNotAcknowledgeOrPurgeData) {
    TestTempDir temp("wal_v2_cp_failure"); const auto path=temp.path()/"graph.wal";
    GraphWAL wal(path); ASSERT_TRUE(wal.open());
    const auto seq=wal.append(WalOpType::AddNode,1);
    std::filesystem::create_directory(path.string()+".checkpoint");
    EXPECT_FALSE(wal.markApplied(seq)); EXPECT_FALSE(wal.lastError().empty());
    EXPECT_EQ(wal.purgeOld(),0U); EXPECT_EQ(wal.append(WalOpType::AddNode,2),UINT64_MAX);
}

TEST(GraphWalV2, MissingOrTornSealedSegmentFailsClosed) {
    for (bool missing : {false,true}) {
        TestTempDir temp("wal_v2_sealed"); const auto path=temp.path()/"graph.wal";
        WalOptions options; options.segment_bytes=GraphWAL::kRecordBytes;
        GraphWAL wal(path,86400,options); ASSERT_TRUE(wal.open());
        ASSERT_EQ(wal.append(WalOpType::AddNode,1),1U);
        ASSERT_EQ(wal.append(WalOpType::AddNode,2),2U); wal.close();
        if (missing) ASSERT_TRUE(std::filesystem::remove(path));
        else std::filesystem::resize_file(path,GraphWAL::kRecordBytes-1);
        EXPECT_FALSE(wal.open()); EXPECT_FALSE(wal.lastError().empty());
    }
}

TEST(GraphWalV2, LegacyMigrationIsExplicitAndUsesFreshContiguousSequenceEpoch) {
    TestTempDir temp("wal_v2_legacy"); const auto path=temp.path()/"graph.wal";
    // Historical little-endian 60-byte layout, with a retention sequence gap.
    { std::ofstream file(path,std::ios::binary);
      for (uint64_t seq : {10U,30U}) {
        std::string raw(60,'\0');
        auto field=[&](size_t at,uint64_t value,size_t width) {
            for (size_t i=0;i<width;++i) raw[at+i]=char(value>>(8*i));
        };
        field(0,seq,8); field(16,1,1); field(20,seq,8); field(48,FLAG_ACTIVE,4);
        field(52,1,1); file.write(raw.data(),raw.size());
      }
    }
    const auto original=bytes(path); GraphWAL rejected(path);
    EXPECT_FALSE(rejected.open()); EXPECT_EQ(bytes(path),original);
    WalOptions options; options.migrate_legacy=true;
    GraphWAL migrated(path,86400,options); ASSERT_TRUE(migrated.open()) << migrated.lastError();
    EXPECT_EQ(bytes(path).substr(0,4),"NXW2");
    const auto pending=migrated.loadUnapplied(); ASSERT_EQ(pending.size(),2U);
    EXPECT_EQ(pending[0].seq,1U); EXPECT_EQ(pending[1].seq,2U);
    EXPECT_EQ(pending[0].node_or_edge_id,10U); EXPECT_EQ(pending[1].node_or_edge_id,30U);
}

TEST(GraphWalV2, LiveGraphAsyncCheckpointEnablesSafeSegmentRetention) {
    TestTempDir temp("wal_v2_async_retention"); const auto path=temp.path()/"graph.wal";
    WalOptions options; options.durability=WalDurability::Async;
    options.segment_bytes=GraphWAL::kRecordBytes;
    GraphStorage storage(temp.path(),"social"); GraphWAL wal(path,-1,options);
    ASSERT_TRUE(storage.open()); ASSERT_TRUE(wal.open());
    LiveGraph graph(&storage,&wal,"social");
    ASSERT_NE(graph.addNode("a","User"),kInvalidDenseId);
    ASSERT_NE(graph.addNode("b","User"),kInvalidDenseId);
    ASSERT_NE(graph.addNode("c","User"),kInvalidDenseId);
    EXPECT_EQ(wal.purgeOld(),0U);
    ASSERT_TRUE(graph.checkpointWAL()); EXPECT_EQ(wal.purgeOld(),2U);
    wal.close(); ASSERT_TRUE(wal.open());
    ASSERT_TRUE(graph.loadFromDisk()); EXPECT_EQ(graph.replayWAL(),0U);
    EXPECT_EQ(graph.activeNodeCount(),3U);
}

TEST(GraphWalV2, NeverTruncatesIncompleteRecordCoveredByDurableCheckpoint) {
    TestTempDir temp("wal_v2_ack_tail"); const auto path=temp.path()/"graph.wal";
    GraphWAL wal(path); ASSERT_TRUE(wal.open());
    ASSERT_TRUE(wal.markApplied(wal.append(WalOpType::AddNode,1))); wal.close();
    std::filesystem::resize_file(path,GraphWAL::kRecordBytes-1);
    EXPECT_FALSE(wal.open());
    EXPECT_EQ(std::filesystem::file_size(path),GraphWAL::kRecordBytes-1);
}

TEST(GraphWalV2, EmptySuccessorAfterRotationAndRetentionCanReopen) {
    TestTempDir temp("wal_v2_empty_successor"); const auto path=temp.path()/"graph.wal";
    GraphWAL wal(path,-1); ASSERT_TRUE(wal.open());
    ASSERT_TRUE(wal.markApplied(wal.append(WalOpType::AddNode,1))); wal.close();
    // Crash boundary after publishing the next segment, before its first append.
    { std::ofstream next(path.string()+".segment.00000000000000000002",std::ios::binary); }
    ASSERT_TRUE(wal.open()); EXPECT_EQ(wal.purgeOld(),1U); wal.close();
    ASSERT_TRUE(wal.open()) << wal.lastError();
    EXPECT_EQ(wal.append(WalOpType::AddNode,2),2U);
}
} // namespace
