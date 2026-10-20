#pragma once
#include "AlgorithmBase.h"
#include <random>
#include <string>
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

        using AdjList = std::vector<std::vector<DenseId>>;
        using SeedSet = std::vector<uint8_t>;
        struct Workspace {
            SeedSet activated;
            std::vector<DenseId> queue;
            const StaticGraph* snapshot;
        };
        static AdjList buildAdjList(const StaticGraph& snapshot);

        static double simulateIC(const AdjList&  adj,
                                 const SeedSet&  seeds,
                                 size_t          R,
                                 double          p,
                                 std::mt19937& rng,Workspace& workspace);

        static size_t runOneSim(const AdjList&  adj,
                                const SeedSet&  seeds,
                                double          p,
                                std::mt19937& rng,Workspace& workspace);

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
