#pragma once
#include "AlgorithmBase.h"
#include <string>
#include <vector>

namespace nexora::graph::algorithms {

    class BetweennessCentrality final : public JobAlgorithm {
    public:
        std::string name() const override;

        AlgoResult run(const StaticGraph&         snapshot,
                       const std::vector<ExtId>& params) override;

    private:
        struct BfsState {
            std::vector<size_t> pred_offsets,pred_counts,pred_storage;
            std::vector<uint64_t> seen;
            std::vector<double>             sigma;
            std::vector<int>                dist;
            std::vector<size_t> queue;
            std::vector<double> delta;

            explicit BfsState(const StaticGraph&);
            void reset(size_t source);
        };

        static void forwardBfs(const StaticGraph&                    snapshot,
                               const std::vector<DenseId>&           nodes,
                               size_t                                s_idx,
                               BfsState&                             state);

        static void backwardPass(size_t                s_idx,
                                 BfsState&       state,
                                 std::vector<double>&  betweenness);

        static void normalize(std::vector<double>& bc, size_t N);

        static std::string buildJson(const StaticGraph&          snapshot,
                                     const std::vector<DenseId>& nodes,
                                     const std::vector<size_t>&  order,
                                     const std::vector<double>&  bc,
                                     size_t                      show_count);
    };

}
