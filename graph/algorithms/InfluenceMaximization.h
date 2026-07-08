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

    private:
        static constexpr size_t kDefaultK = 5;
        static constexpr size_t kDefaultR = 20;
        static constexpr double kDefaultP = 0.1;

        using AdjList = std::unordered_map<DenseId, std::vector<DenseId>>;
        using SeedSet = std::unordered_set<DenseId>;
        static AdjList buildAdjList(const StaticGraph& snapshot);

        static double simulateIC(const AdjList&  adj,
                                 const SeedSet&  seeds,
                                 size_t          R,
                                 double          p,
                                 std::mt19937&   rng);

        static size_t runOneSim(const AdjList&  adj,
                                const SeedSet&  seeds,
                                double          p,
                                std::mt19937&   rng);

        static std::string buildJson(const StaticGraph&          snapshot,
                                     const std::vector<DenseId>& order,
                                     const std::vector<double>&  gains,
                                     double                      final_reach,
                                     size_t                      N,
                                     size_t                      K,
                                     size_t                      R,
                                     double                      p);
    };

}