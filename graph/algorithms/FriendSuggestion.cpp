#include "FriendSuggestion.h"
#include "BuiltinAlgorithms.h"
#include <algorithm>
#include <chrono>
#include <sstream>

namespace nexora::graph::algorithms {


    std::string FriendSuggestion::name() const { return "FriendSuggestion"; }


    AlgoResult FriendSuggestion::run(const LiveGraph&           graph,
                                     const std::vector<ExtId>& params)
    {
        auto t0 = std::chrono::steady_clock::now();


        if (params.empty())
            return AlgoResult{false, "param[0] required: user_id", "", 0.0};

        const ExtId& user_id = params[0];


        DenseId uid = graph.getDenseId(user_id);
        if (uid == kInvalidDenseId)
            return AlgoResult{false, "User not found: " + user_id, "", 0.0};


        size_t limit = kDefaultLimit;
        if (params.size() >= 2 && !params[1].empty()) {
            try   { limit = static_cast<size_t>(std::stoul(params[1])); }
            catch (...) { return AlgoResult{false, "param[1] must be a positive integer", "", 0.0}; }
            if (limit == 0 || limit > kMaxLimit)
                return AlgoResult{false,
                                  "limit must be between 1 and " + std::to_string(kMaxLimit),
                                  "", 0.0};
        }

        TypeId edge_type = kInvalidTypeId;
        if (params.size() >= 3 && !params[2].empty()) {
            auto tid = graph.getEdgeTypeId(params[2]);
            if (!tid)
                return AlgoResult{false, "Unknown edge type: " + params[2], "", 0.0};
            edge_type = *tid;
        }

        FilterSet direct_friends;
        direct_friends.insert(uid);
        gatherNeighbors(graph, uid, edge_type, direct_friends);
        ScoreMap scores = countMutual(graph, direct_friends, uid, edge_type);
        RankedVec ranked = rankAndTrim(scores, limit);

        double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();

        return AlgoResult{true, "", buildJson(graph, user_id, ranked), ms};
    }
    void FriendSuggestion::gatherNeighbors(const LiveGraph& graph,
                                           DenseId          uid,
                                           TypeId           edge_type,
                                           FilterSet&       filter_set)
    {
        auto insert = [&](const AdjEntry& e) -> bool {
            if (edge_type == kInvalidTypeId || e.type_id == edge_type)
                filter_set.insert(e.neighbor);
            return true;
        };
        graph.forEachOutEdge(uid, insert);
        graph.forEachInEdge(uid, insert);
    }

    FriendSuggestion::ScoreMap
    FriendSuggestion::countMutual(const LiveGraph& graph,
                                  const FilterSet& direct_friends,
                                  DenseId          uid,
                                  TypeId           edge_type)
    {
        ScoreMap scores;

        for (DenseId fid : direct_friends) {
            if (fid == uid) continue;

            FilterSet candidates;
            auto visit = [&](const AdjEntry& e) -> bool {
                if ((edge_type == kInvalidTypeId || e.type_id == edge_type) &&
                    !direct_friends.count(e.neighbor))
                    candidates.insert(e.neighbor);
                return true;
            };
            graph.forEachOutEdge(fid, visit);
            graph.forEachInEdge(fid, visit);

            for (DenseId candidate : candidates)
                scores[candidate]++;
        }
        return scores;
    }
    FriendSuggestion::RankedVec
    FriendSuggestion::rankAndTrim(const ScoreMap& scores, size_t limit)
    {
        RankedVec ranked;
        ranked.reserve(scores.size());
        for (const auto& [id, score] : scores)
            ranked.emplace_back(score, id);

        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) {
                      if (a.first != b.first) return a.first > b.first;
                      return a.second < b.second;
                  });

        if (ranked.size() > limit) ranked.resize(limit);
        return ranked;
    }
    std::string FriendSuggestion::buildJson(const LiveGraph&  graph,
                                            const ExtId&      user_id,
                                            const RankedVec&  ranked)
    {
        std::ostringstream j;
        j << "{\"user_id\":\""        << user_id      << "\""
          << ",\"suggestion_count\":" << ranked.size()
          << ",\"suggestions\":[";

        for (size_t i = 0; i < ranked.size(); ++i) {
            if (i) j << ",";
            j << "{\"user_id\":\""     << graph.getExtId(ranked[i].second) << "\""
              << ",\"mutual_friends\":" << ranked[i].first << "}";
        }
        j << "]}";
        return j.str();
    }

    AlgoResult runFriendSuggestion(GraphManager& manager,
                                   const std::string& graph_name,
                                   const std::vector<ExtId>& params)
    {
        FriendSuggestion algo;
        return manager.runLock(graph_name, algo, params);
    }

}
