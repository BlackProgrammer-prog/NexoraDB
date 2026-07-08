//
// Created by Acer on 7/9/2026.
//
#pragma once
#include "AlgorithmBase.h"
#include <deque>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace nexora::graph::algorithms {

    class InfluenceMaximization final : public JobAlgorithm {
    public:
        std::string name() const override;

        AlgoResult run(const StaticGraph&         snapshot,
                       const std::vector<ExtId>& params) override;