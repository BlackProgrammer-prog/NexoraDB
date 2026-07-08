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

    private:
        static constexpr int kMaxDepth = 12;
        using ParentMap = std::unordered_map<DenseId, DenseId>;
        using BfsQueue  = std::deque<DenseId>;

        static bool expandLevel(const LiveGraph& graph,
                                BfsQueue&        q_active,
                                ParentMap&       p_active,
                                const ParentMap& p_other,
                                size_t           level_size,
                                DenseId&         meet_out);

