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

    private:
        struct BfsState {
            std::stack<size_t>              bfs_stack;
            std::vector<std::vector<size_t>> pred;
            std::vector<double>             sigma;
            std::vector<int>                dist;
            std::deque<size_t>              queue;

            explicit BfsState(size_t N, size_t s_idx);
        };

        static void forwardBfs(const StaticGraph&                    snapshot,
                               const std::vector<DenseId>&           nodes,
                               const std::unordered_map<DenseId,size_t>& idx,
                               size_t                                s_idx,
                               BfsState&                             state);
        
        static void backwardPass(size_t                s_idx,
                                 const BfsState&       state,
                                 std::vector<double>&  betweenness);

        static void normalize(std::vector<double>& bc, size_t N);
