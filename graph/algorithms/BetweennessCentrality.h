//
// Created by Acer on 7/9/2026.
//
#pragma once
#include "AlgorithmBase.h"
#include <deque>
#include <stack>
#include <string>
#include <unordered_map>
#include <vector>

namespace nexora::graph::algorithms {

    class BetweennessCentrality final : public JobAlgorithm {
    public:
        std::string name() const override;
        
        AlgoResult run(const StaticGraph&         snapshot,
                       const std::vector<ExtId>& params) override;
