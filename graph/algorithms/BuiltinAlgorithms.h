#pragma once

#include "AlgorithmBase.h"
#include "../GraphManager.h"

namespace nexora {
namespace graph {
namespace algorithms {

AlgoResult runMutualFriends(GraphManager& manager,
                            const std::string& graph_name,
                            const std::vector<ExtId>& params);

AlgoResult runConnectedComponents(GraphManager& manager,
                                  const std::string& graph_name,
                                  const std::vector<ExtId>& params);

AlgoResult runMostConnected(GraphManager& manager,
                            const std::string& graph_name,
                            const std::vector<ExtId>& params);

AlgoResult runNetworkStats(GraphManager& manager,
                           const std::string& graph_name,
                           const std::vector<ExtId>& params);

AlgoResult runCommunityDetection(GraphManager& manager,
                                 const std::string& graph_name,
                                 const std::vector<ExtId>& params);

AlgoResult runAllDistances(GraphManager& manager,
                           const std::string& graph_name,
                           const std::vector<ExtId>& params);

AlgoResult runGetFriends(GraphManager& manager,
                         const std::string& graph_name,
                         const std::vector<ExtId>& params);

AlgoResult runAreConnected(GraphManager& manager,
                           const std::string& graph_name,
                           const std::vector<ExtId>& params);

AlgoResult runShortestPath(GraphManager& manager,
                           const std::string& graph_name,
                           const std::vector<ExtId>& params);

AlgoResult runFriendSuggestion(GraphManager& manager,
                               const std::string& graph_name,
                               const std::vector<ExtId>& params);

AlgoResult runBetweennessCentrality(GraphManager& manager,
                                    const std::string& graph_name,
                                    const std::vector<ExtId>& params);

AlgoResult runInfluenceMaximization(GraphManager& manager,
                                    const std::string& graph_name,
                                    const std::vector<ExtId>& params);

} // namespace algorithms
} // namespace graph
} // namespace nexora
