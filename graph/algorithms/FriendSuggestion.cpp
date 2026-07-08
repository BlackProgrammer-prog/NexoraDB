//
// Created by Acer on 7/9/2026.
//
#include "FriendSuggestion.h"
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
            return AlgoResult{false, "param[0] required: user_id"};

        const ExtId& user_id = params[0];


        DenseId uid = graph.getDenseId(user_id);
        if (uid == kInvalidDenseId)
            return AlgoResult{false, "User not found: " + user_id};


        size_t limit = kDefaultLimit;
        if (params.size() >= 2 && !params[1].empty()) {
            try   { limit = static_cast<size_t>(std::stoul(params[1])); }
            catch (...) { return AlgoResult{false, "param[1] must be a positive integer"}; }
            if (limit == 0 || limit > kMaxLimit)
                return AlgoResult{false,
                                  "limit must be between 1 and " + std::to_string(kMaxLimit)};
        }
        FilterSet direct_friends;
        direct_friends.insert(uid);
        gatherNeighbors(graph, uid, direct_friends);
        ScoreMap scores = countMutual(graph, direct_friends, uid);
        RankedVec ranked = rankAndTrim(scores, limit);

        double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();

        return AlgoResult{true, "", buildJson(graph, user_id, ranked), ms};
    }
    void FriendSuggestion::gatherNeighbors(const LiveGraph& graph,
                                           DenseId          uid,
                                           FilterSet&       filter_set)
    {
        auto insert = [&](const AdjEntry& e) -> bool {
            filter_set.insert(e.neighbor);
            return true;
        };
        graph.forEachOutEdge(uid, insert);
        graph.forEachInEdge(uid, insert);
    }

    FriendSuggestion::ScoreMap
    FriendSuggestion::countMutual(const LiveGraph& graph,
                                  const FilterSet& direct_friends,
                                  DenseId          uid)
    {
        ScoreMap scores;

        for (DenseId fid : direct_friends) {
            if (fid == uid) continue;

            auto visit = [&](const AdjEntry& e) -> bool {
                if (!direct_friends.count(e.neighbor))
                    scores[e.neighbor]++;
                return true;
            };
            graph.forEachOutEdge(fid, visit);
            graph.forEachInEdge(fid, visit);
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
                      if (a.first != b.first) return a.first > b.first;   // نزولی
                      return a.second < b.second;                          // tie-break
                  });

        if (ranked.size() > limit) ranked.resize(limit);
        return ranked;
    }