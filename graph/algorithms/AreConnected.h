#pragma once
#include "AlgorithmBase.h"
#include <chrono>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace nexora::graph::algorithms {

    class AreConnected final : public LockAlgorithm {
    public:
        std::string name() const override;

        AlgoResult run(const LiveGraph&           graph,
                       const std::vector<ExtId>& params) override;

    private:

        static constexpr int kMaxDepth = 12;
        using VisitedMap = std::unordered_map<DenseId, int>;
        using BfsQueue   = std::deque<DenseId>;

        static bool expandLevel(const LiveGraph& graph,
                                BfsQueue&        queue_active,
                                VisitedMap&      visited_active,
                                const VisitedMap& visited_other,
                                size_t           level_size,
                                TypeId           edge_type);


        static std::string buildJson(bool               connected,
                                     int                hops,
                                     const std::string& u1,
                                     const std::string& u2);


        static double elapsedMs(
                const std::chrono::steady_clock::time_point& t0);
    };

}

