//
// Created by Acer on 7/9/2026.
//
#pragma once
#include "AlgorithmBase.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nexora::graph::algorithms {

    class FriendSuggestion final : public LockAlgorithm {
    public:
        std::string name() const override;

        AlgoResult run(const LiveGraph&           graph,
                       const std::vector<ExtId>& params) override;
