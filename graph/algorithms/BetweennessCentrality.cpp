//
// Created by Acer on 7/9/2026.
//
#include "BetweennessCentrality.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <numeric>
#include <sstream>
namespace nexora::graph::algorithms {

    BetweennessCentrality::BfsState::BfsState(size_t N, size_t s_idx)
    : pred(N), sigma(N, 0.0), dist(N, -1)
    {
        sigma[s_idx] = 1.0;
        dist[s_idx]  = 0;
        queue.push_back(s_idx);
    }
    std::string BetweennessCentrality::name() const {
        return "BetweennessCentrality";
    }

    AlgoResult BetweennessCentrality::run(const StaticGraph&         snapshot,
                                          const std::vector<ExtId>& params)
    {
        auto t0 = std::chrono::steady_clock::now();

        size_t top_k = SIZE_MAX;
        if (!params.empty() && !params[0].empty()) {
            try   { top_k = static_cast<size_t>(std::stoul(params[0])); }
            catch (...) { return AlgoResult{false, "param[0] must be a positive integer (top_k)"}; }
            if (top_k == 0) return AlgoResult{false, "top_k must be >= 1"};
        }

        std::vector<DenseId> nodes;
        nodes.reserve(snapshot.nodeCount());
        snapshot.forEachNode([&](DenseId id, TypeId) -> bool {
            nodes.push_back(id);
            return true;
        });

        const size_t N = nodes.size();
        if (N == 0)
            return AlgoResult{true, "", "{\"total_nodes\":0,\"nodes\":[]}", 0.0};

        std::unordered_map<DenseId, size_t> idx;
        idx.reserve(N);
        for (size_t i = 0; i < N; ++i)
            idx[nodes[i]] = i;

        std::vector<double> betweenness(N, 0.0);

        for (size_t s = 0; s < N; ++s) {
            BfsState state(N, s);
            forwardBfs(snapshot, nodes, idx, s, state);
            backwardPass(s, state, betweenness);
        }

        normalize(betweenness, N);

        std::vector<size_t> order(N);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(),
                  [&](size_t a, size_t b) { return betweenness[a] > betweenness[b]; });

        const size_t show = std::min(top_k, N);
        double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();

        return AlgoResult{true, "", buildJson(snapshot, nodes, order, betweenness, show), ms};
    }

