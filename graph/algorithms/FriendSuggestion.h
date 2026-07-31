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
    private:
        static constexpr size_t kDefaultLimit = 10;
        static constexpr size_t kMaxLimit     = 1'000;

        using FilterSet     = std::unordered_set<DenseId>;
        using ScoreMap      = std::unordered_map<DenseId, int>;
        using RankedVec     = std::vector<std::pair<int, DenseId>>;

        static void gatherNeighbors(const LiveGraph& graph,
                                    DenseId          uid,
                                    TypeId           edge_type,
                                    FilterSet&       filter_set);

        static ScoreMap countMutual(const LiveGraph& graph,
                                    const FilterSet& direct_friends,
                                    DenseId          uid,
                                    TypeId           edge_type);

        static RankedVec rankAndTrim(const ScoreMap& scores, size_t limit);

        static std::string buildJson(const LiveGraph&  graph,
                                     const ExtId&      user_id,
                                     const RankedVec&  ranked);
    };

}
