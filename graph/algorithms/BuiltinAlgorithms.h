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

} // namespace algorithms
} // namespace graph
} // namespace nexora
