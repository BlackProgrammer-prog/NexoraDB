#include "TestTempDir.h"

#include "graph/Graphstorage.h"
#include "graph/Graphwal.h"
#include "graph/Livegraph.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

    using namespace nexora::graph;

    TEST(GraphSmoke, OpensAndClosesStorageAndWal) {
        TestTempDir temp("nexora_graph_smoke");

        GraphStorage storage(temp.path(), "social");
        GraphWAL wal(temp.path() / "graph.wal");

        ASSERT_TRUE(storage.open());
        ASSERT_TRUE(storage.isOpen());

        ASSERT_TRUE(wal.open());
        ASSERT_TRUE(wal.isOpen());

        wal.close();
        storage.close();

        EXPECT_FALSE(wal.isOpen());
        EXPECT_FALSE(storage.isOpen());
    }

    TEST(LiveGraphCrud, AddsTraversesAndRemovesNodesAndEdges) {
        LiveGraph graph(nullptr, nullptr, "memory");

        const DenseId u1 = graph.addNode("u1", "User");
        const DenseId u2 = graph.addNode("u2", "User");

        ASSERT_NE(u1, kInvalidDenseId);
        ASSERT_NE(u2, kInvalidDenseId);

        EXPECT_TRUE(graph.hasNode("u1"));
        EXPECT_TRUE(graph.hasNode("u2"));
        EXPECT_EQ(graph.activeNodeCount(), 2U);

        const EdgeId edge =
                graph.addEdge("u1", "u2", "FOLLOWS", true);

        ASSERT_NE(edge, kInvalidEdgeId);
        EXPECT_TRUE(graph.hasEdge("u1", "u2", "FOLLOWS"));
        EXPECT_EQ(graph.activeEdgeCount(), 1U);

        const auto neighbors =
                graph.neighborsExt("u1", Direction::Out, "FOLLOWS");

        ASSERT_EQ(neighbors.size(), 1U);
        EXPECT_EQ(neighbors.front(), "u2");

        EXPECT_TRUE(graph.removeEdge(edge));
        EXPECT_FALSE(graph.hasEdge("u1", "u2", "FOLLOWS"));
        EXPECT_EQ(graph.activeEdgeCount(), 0U);

        EXPECT_TRUE(graph.removeNode("u2"));
        EXPECT_FALSE(graph.hasNode("u2"));
        EXPECT_EQ(graph.activeNodeCount(), 1U);
    }

    TEST(GraphPersistence, ReloadsNodeAndEdgeCountsFromDisk) {
        TestTempDir temp("nexora_graph_reload");

        {
            GraphStorage storage(temp.path(), "social");
            GraphWAL wal(temp.path() / "graph.wal");

            ASSERT_TRUE(storage.open());
            ASSERT_TRUE(wal.open());

            LiveGraph graph(&storage, &wal, "social");

            ASSERT_NE(graph.addNode("u1", "User"), kInvalidDenseId);
            ASSERT_NE(graph.addNode("u2", "User"), kInvalidDenseId);
            ASSERT_NE(
                    graph.addEdge("u1", "u2", "FOLLOWS", true),
                    kInvalidEdgeId
            );

            EXPECT_EQ(graph.activeNodeCount(), 2U);
            EXPECT_EQ(graph.activeEdgeCount(), 1U);
        }

        {
            GraphStorage storage(temp.path(), "social");
            GraphWAL wal(temp.path() / "graph.wal");

            ASSERT_TRUE(storage.open());
            ASSERT_TRUE(wal.open());

            LiveGraph reloaded(&storage, &wal, "social");
            ASSERT_TRUE(reloaded.loadFromDisk());

            EXPECT_EQ(reloaded.activeNodeCount(), 2U);
            EXPECT_EQ(reloaded.activeEdgeCount(), 1U);
        }
    }

    TEST(GraphWal, ReplaysUnappliedRecord) {
        TestTempDir temp("nexora_graph_wal");

        GraphWAL wal(temp.path() / "graph.wal");
        ASSERT_TRUE(wal.open());

        const auto sequence = wal.append(
                WalOpType::AddNode,
                0,
                0,
                0,
                1,
                FLAG_ACTIVE
        );

        ASSERT_NE(sequence, UINT64_MAX);
        ASSERT_EQ(wal.loadUnapplied().size(), 1U);

        LiveGraph graph(nullptr, &wal, "replay");
        EXPECT_EQ(graph.replayWAL(), 1U);
        EXPECT_EQ(graph.activeNodeCount(), 1U);
        EXPECT_TRUE(graph.getNode(DenseId{0}).has_value());
        EXPECT_TRUE(wal.loadUnapplied().empty());
    }

/*
 * این تست در نسخه فعلی احتمالاً FAIL می‌شود و عمداً regression test است.
 * compactEdges فعلی src/dst را با node_remap روی دیسک بازنویسی نمی‌کند.
 */
    TEST(GraphCompaction, PersistsRemappedEdgeEndpoints) {
        TestTempDir temp("nexora_graph_compaction");

        GraphStorage storage(temp.path(), "social");
        ASSERT_TRUE(storage.open());

        LiveGraph graph(&storage, nullptr, "social");

        ASSERT_NE(graph.addNode("u0", "User"), kInvalidDenseId);
        ASSERT_NE(graph.addNode("removed", "User"), kInvalidDenseId);
        ASSERT_NE(graph.addNode("u2", "User"), kInvalidDenseId);

        ASSERT_NE(
                graph.addEdge("u0", "u2", "FOLLOWS", true),
                kInvalidEdgeId
        );

        ASSERT_TRUE(graph.removeNode("removed"));
        ASSERT_TRUE(graph.compact());

        EXPECT_EQ(graph.activeNodeCount(), 2U);
        EXPECT_EQ(graph.activeEdgeCount(), 1U);

        bool inspected_edge = false;

        storage.scanAllEdges([&](const EdgeRecord& edge) {
            if (!edge.isActive()) {
                return true;
            }

            inspected_edge = true;

            // پس از compaction هر دو endpoint باید در بازه nodeهای جدید باشند.
            EXPECT_LT(edge.src, graph.activeNodeCount());
            EXPECT_LT(edge.dst, graph.activeNodeCount());
            return true;
        });

        EXPECT_TRUE(inspected_edge);
    }

} // namespace