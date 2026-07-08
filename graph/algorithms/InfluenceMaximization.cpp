//
// Created by Acer on 7/9/2026.
//
#include "InfluenceMaximization.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <random>
namespace nexora::graph::algorithms {

    std::string InfluenceMaximization::name() const {
        return "InfluenceMaximization";
    }

    AlgoResult InfluenceMaximization::run(const StaticGraph&         snapshot,
                                          const std::vector<ExtId>& params)
    {
        auto t0 = std::chrono::steady_clock::now();

        size_t K = kDefaultK;
        size_t R = kDefaultR;
        double p = kDefaultP;

        if (!params.empty() && !params[0].empty()) {
            try   { K = static_cast<size_t>(std::stoul(params[0])); }
            catch (...) { return AlgoResult{false, "param[0] must be positive integer (K)"}; }
        }
        if (params.size() >= 2 && !params[1].empty()) {
            try   { R = static_cast<size_t>(std::stoul(params[1])); }
            catch (...) { return AlgoResult{false, "param[1] must be positive integer (R)"}; }
        }
        if (K == 0) return AlgoResult{false, "K must be >= 1"};
        if (R == 0) return AlgoResult{false, "R must be >= 1"};

        std::vector<DenseId> all_nodes;
        all_nodes.reserve(snapshot.nodeCount());
        snapshot.forEachNode([&](DenseId id, TypeId) -> bool {
            all_nodes.push_back(id);
            return true;
        });

        const size_t N = all_nodes.size();
        if (N == 0)
            return AlgoResult{true, "",
                              "{\"k_seeds\":0,\"seeds\":[],\"estimated_reach\":0}", 0.0};

        K = std::min(K, N);
        AdjList adj = buildAdjList(snapshot);
        std::mt19937 rng(42u);
        SeedSet               chosen;
        std::vector<DenseId>  order;
        std::vector<double>   gains;
        double                cur_reach = 0.0;

        order.reserve(K);
        gains.reserve(K);

        for (size_t k = 0; k < K; ++k) {
            DenseId best      = kInvalidDenseId;
            double  best_gain = -1.0;

            for (DenseId c : all_nodes) {
                if (chosen.count(c)) continue;

                chosen.insert(c);
                const double new_reach = simulateIC(adj, chosen, R, p, rng);
                const double gain      = new_reach - cur_reach;
                chosen.erase(c);

                if (gain > best_gain) {
                    best_gain = gain;
                    best      = c;
                }
            }
            if (best == kInvalidDenseId) break;

            chosen.insert(best);
            order.push_back(best);
            gains.push_back(best_gain);
            cur_reach += best_gain;
        }
        const double final_reach = simulateIC(adj, chosen, R, p, rng);

        double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();

        return AlgoResult{true, "",
                          buildJson(snapshot, order, gains, final_reach, N, K, R, p),
                          ms};
    }

    InfluenceMaximization::AdjList
    InfluenceMaximization::buildAdjList(const StaticGraph& snapshot)
    {
        AdjList adj;
        adj.reserve(snapshot.nodeCount());

        snapshot.forEachEdge(
                [&](EdgeId, DenseId src, DenseId dst, TypeId) -> bool {
                    adj[src].push_back(dst);
                    adj[dst].push_back(src);   // undirected
                    return true;
                });

        return adj;
    }
    double InfluenceMaximization::simulateIC(const AdjList& adj,
                                             const SeedSet& seeds,
                                             size_t         R,
                                             double         p,
                                             std::mt19937&  rng)
    {
        double total = 0.0;
        for (size_t r = 0; r < R; ++r)
            total += static_cast<double>(runOneSim(adj, seeds, p, rng));
        return total / static_cast<double>(R);
    }
    size_t InfluenceMaximization::runOneSim(const AdjList& adj,
                                            const SeedSet& seeds,
                                            double         p,
                                            std::mt19937&  rng)
    {
        std::uniform_real_distribution<double> dist01(0.0, 1.0);

        SeedSet               activated(seeds.begin(), seeds.end());
        std::deque<DenseId>   queue(seeds.begin(), seeds.end());

        while (!queue.empty()) {
            DenseId v = queue.front();
            queue.pop_front();

            auto it = adj.find(v);
            if (it == adj.end()) continue;

            for (DenseId nbr : it->second) {
                if (!activated.count(nbr) && dist01(rng) < p) {
                    activated.insert(nbr);
                    queue.push_back(nbr);
                }
            }
        }
        return activated.size();
    }

