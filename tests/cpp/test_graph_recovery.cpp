#include "TestTempDir.h"
#include "graph/GraphManager.h"
#include <gtest/gtest.h>
using namespace nexora::graph;
TEST(GraphRecovery, RefusesIncompleteBuildAndOrphanDirectory) {
    TestTempDir temp("graph_recovery");
    nexora::core::DocEngine source((temp.path()/"documents").string());
    const auto directory=temp.path()/"graphs";
    {
        GraphManager manager(&source,directory); GraphDefinition definition; definition.name="social";
        ASSERT_TRUE(manager.createGraph(definition));
    }
    {
        GraphStorage storage(directory/"social","social"); ASSERT_TRUE(storage.open());
        ASSERT_TRUE(storage.updateMetaState(GraphState::Building));
    }
    { GraphManager manager(&source,directory); EXPECT_THROW(manager.recoverStoredGraphs(),std::runtime_error); }
    {
        GraphStorage storage(directory/"social","social"); ASSERT_TRUE(storage.open());
        ASSERT_TRUE(storage.updateMetaState(GraphState::Clean));
    }
    std::filesystem::create_directory(directory/"orphan");
    { GraphManager manager(&source,directory); EXPECT_THROW(manager.recoverStoredGraphs(),std::runtime_error); }
}
