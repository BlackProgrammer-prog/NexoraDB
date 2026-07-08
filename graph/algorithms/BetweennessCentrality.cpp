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

    void BetweennessCentrality::forwardBfs(
            const StaticGraph&                    snapshot,
            const std::vector<DenseId>&           nodes,
            const std::unordered_map<DenseId,size_t>& idx,
            size_t                                s_idx,
            BfsState&                             st)
    {
        while (!st.queue.empty()) {
            size_t v = st.queue.front();
            st.queue.pop_front();
            st.bfs_stack.push(v);

            auto expand = [&](DenseId nbr_id) -> bool {
                auto it = idx.find(nbr_id);
                if (it == idx.end()) return true;
                size_t w = it->second;

                if (st.dist[w] < 0) {
                    st.dist[w] = st.dist[v] + 1;
                    st.queue.push_back(w);
                }
                if (st.dist[w] == st.dist[v] + 1) {
                    st.sigma[w] += st.sigma[v];
                    st.pred[w].push_back(v);
                }
                return true;
            };

            snapshot.forEachNeighbor(nodes[v], Direction::Out,
                                     [&](DenseId n, TypeId) -> bool { return expand(n); });
            snapshot.forEachNeighbor(nodes[v], Direction::In,
                                     [&](DenseId n, TypeId) -> bool { return expand(n); });
        }
    }
    void BetweennessCentrality::backwardPass(size_t     s_idx,
    const BfsState&      st,
            std::vector<double>& betweenness)
{
    const size_t N = betweenness.size();
    std::vector<double> delta(N, 0.0);

    auto stk = st.bfs_stack;

    while (!stk.empty()) {
    size_t w = stk.top();
    stk.pop();
    for (size_t v : st.pred[w]) {
    delta[v] += (st.sigma[v] / st.sigma[w]) * (1.0 + delta[w]);
}
if (w != s_idx)
betweenness[w] += delta[w];
}
}


void BetweennessCentrality::normalize(std::vector<double>& bc, size_t N)
{
    const double norm = (N > 2) ? (2.0 / ((N - 1.0) * (N - 2.0))) : 1.0;
    for (auto& v : bc) v *= 0.5 * norm;
}

std::string BetweennessCentrality::buildJson(
        const StaticGraph&          snapshot,
        const std::vector<DenseId>& nodes,
        const std::vector<size_t>&  order,
        const std::vector<double>&  bc,
        size_t                      show_count)
{
    std::ostringstream j;
    j << std::fixed << std::setprecision(6);
    j << "{\"total_nodes\":" << nodes.size()
      << ",\"showing\":"     << show_count
      << ",\"nodes\":[";

    for (size_t r = 0; r < show_count; ++r) {
        const size_t i = order[r];
        if (r) j << ",";
        j << "{\"user_id\":\""  << snapshot.extId(nodes[i]) << "\""
          << ",\"betweenness\":" << bc[i]
          << ",\"rank\":"        << (r + 1) << "}";
    }
    j << "]}";
    return j.str();
}

}