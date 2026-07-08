//
// Created by Acer on 7/8/2026.
//

#pragma once
#include "AlgorithmBase.h"
#include <string>
#include <vector>

namespace nexora::graph::algorithms {

    class GetFriends final : public LockAlgorithm {
    public:

        std::string name() const override;


        AlgoResult run(const LiveGraph&            graph,
                       const std::vector<ExtId>&  params) override;

    private:

        static constexpr size_t kMaxLimit = 10'000;


        static void collectNeighbors(const LiveGraph&      graph,
                                     DenseId               uid,
                                     TypeId                edge_type,
                                     size_t                limit,
                                     std::vector<DenseId>& out_vec);


        static std::string buildJson(const LiveGraph&            graph,
                                     const ExtId&                user_id,
                                     const std::vector<DenseId>& friends,
                                     bool                        limit_applied);
    };

}
