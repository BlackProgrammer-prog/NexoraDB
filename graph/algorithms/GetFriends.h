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