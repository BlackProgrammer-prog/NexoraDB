#include "AreConnected.h"
#include "BuiltinAlgorithms.h"
#include <sstream>


namespace nexora::graph::algorithms {


    std::string AreConnected::name() const { return "AreConnected"; }


    AlgoResult AreConnected::run(const LiveGraph&           graph,
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
        std::string edge_type_name;
        if (params.size() >= 3 && !params[2].empty()) {
            auto tid = graph.getEdgeTypeId(params[2]);
            if (!tid)
                return AlgoResult{false, "Unknown edge type: " + params[2], "", 0.0};
            edge_type = *tid;
            edge_type_name = params[2];
        }


        if (src == dst)
            return AlgoResult{true, "", buildJson(true, 0, params[0], params[1]),
                              elapsedMs(t0)};


        if (graph.hasEdge(params[0], params[1], edge_type_name) ||
            graph.hasEdge(params[1], params[0], edge_type_name))
            return AlgoResult{true, "", buildJson(true, 1, params[0], params[1]),
                              elapsedMs(t0)};

        VisitedMap visited_fwd, visited_bwd;
        BfsQueue   queue_fwd,   queue_bwd;

        visited_fwd[src] = 0;   queue_fwd.push_back(src);
        visited_bwd[dst] = 0;   queue_bwd.push_back(dst);

        for (int depth = 1; depth <= kMaxDepth; ++depth) {
            if (queue_fwd.empty() || queue_bwd.empty()) break;


            const bool expand_fwd = (queue_fwd.size() <= queue_bwd.size());

            BfsQueue&    q_a = expand_fwd ? queue_fwd  : queue_bwd;
            VisitedMap&  v_a = expand_fwd ? visited_fwd: visited_bwd;
            const VisitedMap& v_o = expand_fwd ? visited_bwd: visited_fwd;

            if (expandLevel(graph, q_a, v_a, v_o, q_a.size(), edge_type))
                return AlgoResult{true, "", buildJson(true, depth, params[0], params[1]),
                                  elapsedMs(t0)};
        }

        return AlgoResult{true, "", buildJson(false, -1, params[0], params[1]),
                          elapsedMs(t0)};
    }

    bool AreConnected::expandLevel(const LiveGraph&  graph,
                                   BfsQueue&         q_a,
                                   VisitedMap&       v_a,
                                   const VisitedMap& v_o,
                                   size_t            level_size,
                                   TypeId            edge_type)
    {
        for (size_t qi = 0; qi < level_size; ++qi) {
            DenseId cur = q_a.front();
            q_a.pop_front();

            bool hit = false;

            auto check = [&](DenseId nbr) -> bool {
                if (v_o.count(nbr)) { hit = true; return false; }
                if (!v_a.count(nbr)) {
                    v_a[nbr] = v_a[cur] + 1;
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
    std::string AreConnected::buildJson(bool               connected,
                                        int                hops,
                                        const std::string& u1,
                                        const std::string& u2)
    {
        std::ostringstream j;
        j << "{\"connected\":"  << (connected ? "true" : "false")
          << ",\"hops\":"       << hops
          << ",\"user1\":\""    << u1 << "\""
          << ",\"user2\":\""    << u2 << "\""
          << "}";
        return j.str();
    }
    double AreConnected::elapsedMs(
            const std::chrono::steady_clock::time_point& t0)
    {
        return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
    }

    AlgoResult runAreConnected(GraphManager& manager,
                               const std::string& graph_name,
                               const std::vector<ExtId>& params)
    {
        AreConnected algo;
        return manager.runLock(graph_name, algo, params);
    }

}
