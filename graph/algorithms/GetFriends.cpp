#include "GetFriends.h"
#include "BuiltinAlgorithms.h"
#include <algorithm>
#include <chrono>
#include <sstream>
namespace nexora::graph::algorithms {


    std::string GetFriends::name() const { return "GetFriends"; }


    AlgoResult GetFriends::run(const LiveGraph&           graph,
                               const std::vector<ExtId>& params)
    {
        auto t0 = std::chrono::steady_clock::now();


        if (params.empty())
            return AlgoResult{false, "param[0] required: user_id", "", 0.0};

        const ExtId& user_id = params[0];


        DenseId uid = graph.getDenseId(user_id);
        if (uid == kInvalidDenseId)
            return AlgoResult{false, "User not found: " + user_id, "", 0.0};


        size_t limit = SIZE_MAX;
        if (params.size() >= 2 && !params[1].empty()) {
            try   { limit = static_cast<size_t>(std::stoul(params[1])); }
            catch (...) { return AlgoResult{false, "param[1] must be a positive integer", "", 0.0}; }
            if (limit == 0)
                return AlgoResult{false, "limit must be > 0", "", 0.0};
            if (limit > kMaxLimit)
                return AlgoResult{false, "limit exceeds maximum (" + std::to_string(kMaxLimit) + ")", "", 0.0};
        }


        TypeId edge_type = kInvalidTypeId;
        if (params.size() >= 3 && !params[2].empty()) {
            auto tid = graph.getEdgeTypeId(params[2]);
            if (!tid)
                return AlgoResult{false, "Unknown edge type: " + params[2], "", 0.0};
            edge_type = *tid;
        }


        std::vector<DenseId> friends;
        collectNeighbors(graph, uid, edge_type, SIZE_MAX, friends);


        std::sort(friends.begin(), friends.end());
        friends.erase(std::unique(friends.begin(), friends.end()), friends.end());
        const bool limit_applied = (friends.size() > limit);
        if (friends.size() > limit) friends.resize(limit);


        double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();

        return AlgoResult{true, "", buildJson(graph, user_id, friends, limit_applied), ms};
    }
    void GetFriends::collectNeighbors(const LiveGraph&      graph,
                                      DenseId               uid,
                                      TypeId                edge_type,
                                      size_t                limit,
                                      std::vector<DenseId>& out_vec)
    {
        auto accept = [&](const AdjEntry& e) -> bool {
            if (out_vec.size() >= limit) return false;
            if (edge_type == kInvalidTypeId || e.type_id == edge_type)
                out_vec.push_back(e.neighbor);
            return true;
        };

        graph.forEachOutEdge(uid, accept);

        if (out_vec.size() < limit)
            graph.forEachInEdge(uid, accept);
    }


    std::string GetFriends::buildJson(const LiveGraph&            graph,
                                      const ExtId&                user_id,
                                      const std::vector<DenseId>& friends,
                                      bool                        limit_applied)
    {
        std::ostringstream j;
        j << "{\"user_id\":\""    << user_id         << "\""
          << ",\"friend_count\":" << friends.size()
          << ",\"limit_applied\":" << (limit_applied ? "true" : "false")
          << ",\"friends\":[";

        for (size_t i = 0; i < friends.size(); ++i) {
            if (i) j << ",";
            j << "\"" << graph.getExtId(friends[i]) << "\"";
        }
        j << "]}";
        return j.str();
    }

    AlgoResult runGetFriends(GraphManager& manager,
                             const std::string& graph_name,
                             const std::vector<ExtId>& params)
    {
        GetFriends algo;
        return manager.runLock(graph_name, algo, params);
    }

}
