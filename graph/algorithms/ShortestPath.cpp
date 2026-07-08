//
// Created by Acer on 7/9/2026.
//
#include "ShortestPath.h"
#include <algorithm>
#include <sstream>


namespace nexora::graph::algorithms {


    std::string ShortestPath::name() const { return "ShortestPath"; }


    AlgoResult ShortestPath::run(const LiveGraph&           graph,
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