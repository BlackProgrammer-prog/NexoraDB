//
// Created by Acer on 7/9/2026.
//
#include "AreConnected.h"
#include <sstream>


namespace nexora::graph::algorithms {


    std::string AreConnected::name() const { return "AreConnected"; }


    AlgoResult AreConnected::run(const LiveGraph&           graph,
                                 const std::vector<ExtId>& params)
    {
        auto t0 = std::chrono::steady_clock::now();


        if (params.size() < 2)
            return AlgoResult{false, "Need 2 user IDs: [user1_id, user2_id]"};


        DenseId src = graph.getDenseId(params[0]);
        DenseId dst = graph.getDenseId(params[1]);

        if (src == kInvalidDenseId)
            return AlgoResult{false, "User not found: " + params[0]};
        if (dst == kInvalidDenseId)
            return AlgoResult{false, "User not found: " + params[1]};


        if (src == dst)
            return AlgoResult{true, "", buildJson(true, 0, params[0], params[1]),
                              elapsedMs(t0)};


        if (graph.hasEdge(params[0], params[1], "") ||
            graph.hasEdge(params[1], params[0], ""))
            return AlgoResult{true, "", buildJson(true, 1, params[0], params[1]),
                              elapsedMs(t0)};

        VisitedMap visited_fwd, visited_bwd;
        BfsQueue   queue_fwd,   queue_bwd;

        visited_fwd[src] = 0;   queue_fwd.push_back(src);
        visited_bwd[dst] = 0;   queue_bwd.push_back(dst);
