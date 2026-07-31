#include "ShortestPath.h"
#include "BuiltinAlgorithms.h"
#include <algorithm>
#include <sstream>

namespace nexora::graph::algorithms {


    std::string ShortestPath::name() const { return "ShortestPath"; }


    AlgoResult ShortestPath::run(const LiveGraph&           graph,
                                 const std::vector<ExtId>& params)
    {
        auto t0 = std::chrono::steady_clock::now();


        if (params.size() < 2)
            return AlgoResult{false, "Need 2 user IDs: [user1_id, user2_id]", "", 0.0};


        DenseId src = graph.getDenseId(params[0]);
        DenseId dst = graph.getDenseId(params[1]);

        if (src == kInvalidDenseId)
            return AlgoResult{false, "User not found: " + params[0], "", 0.0};
        if (dst == kInvalidDenseId)
            return AlgoResult{false, "User not found: " + params[1], "", 0.0};

        TypeId edge_type = kInvalidTypeId;
        if (params.size() >= 3 && !params[2].empty()) {
            auto tid = graph.getEdgeTypeId(params[2]);
            if (!tid)
                return AlgoResult{false, "Unknown edge type: " + params[2], "", 0.0};
            edge_type = *tid;
        }


        if (src == dst)
            return AlgoResult{true, "",
                              buildJson(graph, {src}),
                              elapsedMs(t0)};

        ParentMap parent_fwd, parent_bwd;
        BfsQueue  queue_fwd,  queue_bwd;

        parent_fwd[src] = kInvalidDenseId;
        parent_bwd[dst] = kInvalidDenseId;
        queue_fwd.push_back(src);
        queue_bwd.push_back(dst);

        DenseId meet = kInvalidDenseId;

        for (int depth = 1; depth <= kMaxDepth; ++depth) {
            if (queue_fwd.empty() || queue_bwd.empty()) break;

            const bool expand_fwd = (queue_fwd.size() <= queue_bwd.size());

            BfsQueue&       q_a = expand_fwd ? queue_fwd  : queue_bwd;
            ParentMap&      p_a = expand_fwd ? parent_fwd : parent_bwd;
            const ParentMap& p_o = expand_fwd ? parent_bwd : parent_fwd;

            if (expandLevel(graph, q_a, p_a, p_o, q_a.size(), edge_type, meet))
                break;
        }

        if (meet == kInvalidDenseId) {
            std::ostringstream j;
            j << "{\"found\":false,\"hops\":-1,\"path\":[]"
              << ",\"user1\":\"" << params[0] << "\""
              << ",\"user2\":\"" << params[1] << "\"}";
            return AlgoResult{true, "", j.str(), elapsedMs(t0)};
        }
        auto path_left  = rebuildLeft (meet, parent_fwd);
        auto path_right = rebuildRight(meet, parent_bwd);

        path_left.insert(path_left.end(), path_right.begin(), path_right.end());

        return AlgoResult{true, "", buildJson(graph, path_left), elapsedMs(t0)};
    }
    bool ShortestPath::expandLevel(const LiveGraph& graph,
                                   BfsQueue&        q_a,
                                   ParentMap&       p_a,
                                   const ParentMap& p_o,
                                   size_t           level_size,
                                   TypeId           edge_type,
                                   DenseId&         meet_out)
    {
        for (size_t qi = 0; qi < level_size; ++qi) {
            DenseId cur = q_a.front();
            q_a.pop_front();

            bool hit = false;

            auto check = [&](DenseId nbr) -> bool {
                if (p_o.count(nbr)) {
                    if (!p_a.count(nbr))
                        p_a[nbr] = cur;
                    meet_out = nbr;
                    hit = true;
                    return false;
                }
                if (!p_a.count(nbr)) {
                    p_a[nbr] = cur;
                    q_a.push_back(nbr);
                }
                return true;
            };

            graph.forEachOutEdge(cur, [&](const AdjEntry& e) -> bool {
                if (edge_type != kInvalidTypeId && e.type_id != edge_type) return true;
                return check(e.neighbor);
            });
            if (hit) return true;

            graph.forEachInEdge(cur, [&](const AdjEntry& e) -> bool {
                if (edge_type != kInvalidTypeId && e.type_id != edge_type) return true;
                return check(e.neighbor);
            });
            if (hit) return true;
        }
        return false;
    }
    std::vector<DenseId> ShortestPath::rebuildLeft(DenseId           meet,
                                                   const ParentMap&  parent_fwd)
    {
        std::vector<DenseId> half;
        for (DenseId v = meet; v != kInvalidDenseId; ) {
            half.push_back(v);
            auto it = parent_fwd.find(v);
            v = (it != parent_fwd.end()) ? it->second : kInvalidDenseId;
        }
        std::reverse(half.begin(), half.end());
        return half;
    }

    std::vector<DenseId> ShortestPath::rebuildRight(DenseId          meet,
                                                    const ParentMap& parent_bwd)
    {
        std::vector<DenseId> half;
        auto start_it = parent_bwd.find(meet);
        DenseId v = (start_it != parent_bwd.end()) ? start_it->second : kInvalidDenseId;

        while (v != kInvalidDenseId) {
            half.push_back(v);
            auto it = parent_bwd.find(v);
            v = (it != parent_bwd.end()) ? it->second : kInvalidDenseId;
        }
        return half;
    }
    std::string ShortestPath::buildJson(const LiveGraph&            graph,
                                        const std::vector<DenseId>& path)
    {
        const int hops = path.empty() ? -1 : static_cast<int>(path.size()) - 1;
        std::ostringstream j;
        j << "{\"found\":true"
          << ",\"hops\":"  << hops
          << ",\"path\":[";
        for (size_t i = 0; i < path.size(); ++i) {
            if (i) j << ",";
            j << "\"" << graph.getExtId(path[i]) << "\"";
        }
        j << "]}";
        return j.str();
    }
    double ShortestPath::elapsedMs(
            const std::chrono::steady_clock::time_point& t0)
    {
        return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
    }

    AlgoResult runShortestPath(GraphManager& manager,
                               const std::string& graph_name,
                               const std::vector<ExtId>& params)
    {
        ShortestPath algo;
        return manager.runLock(graph_name, algo, params);
    }

}
