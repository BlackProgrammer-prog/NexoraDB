//
// Created by Acer on 7/9/2026.
//
#pragma once
#include "AlgorithmBase.h"
#include <chrono>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace nexora::graph::algorithms {

    class ShortestPath final : public LockAlgorithm {
    public:
        std::string name() const override;

        AlgoResult run(const LiveGraph&           graph,
                       const std::vector<ExtId>& params) override;
