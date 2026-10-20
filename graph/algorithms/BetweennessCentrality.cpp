#include "BetweennessCentrality.h"
#include "BuiltinAlgorithms.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <numeric>
#include <sstream>
namespace nexora::graph::algorithms {

    BetweennessCentrality::BfsState::BfsState(const StaticGraph& snapshot)
    :pred_offsets(snapshot.nodeCount()+1,0),pred_counts(snapshot.nodeCount()),
     seen(snapshot.nodeCount(),UINT64_MAX),sigma(snapshot.nodeCount()),dist(snapshot.nodeCount()),delta(snapshot.nodeCount())
    {
        for (size_t i=0;i<snapshot.nodeCount();++i)
            pred_offsets[i+1]=pred_offsets[i]+snapshot.outDegree(i)+snapshot.inDegree(i);
        pred_storage.resize(pred_offsets.back()); queue.reserve(snapshot.nodeCount());
    }
    void BetweennessCentrality::BfsState::reset(size_t s_idx) {
        std::fill(sigma.begin(),sigma.end(),0); std::fill(dist.begin(),dist.end(),-1);
        std::fill(delta.begin(),delta.end(),0); std::fill(pred_counts.begin(),pred_counts.end(),0);
        std::fill(seen.begin(),seen.end(),UINT64_MAX); queue.clear();
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
            catch (...) { return AlgoResult{false, "param[0] must be a positive integer (top_k)", "", 0.0}; }
            if (top_k == 0) return AlgoResult{false, "top_k must be >= 1", "", 0.0};
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

        std::vector<double> betweenness(N, 0.0);
        (void)snapshot.incoming(0); // Allocate optional CSC outside the node loop.
        BfsState state(snapshot);
        for (size_t s = 0; s < N; ++s) {
            snapshot.checkCancelled(); state.reset(s);
            forwardBfs(snapshot, nodes, s, state);
            backwardPass(s, state, betweenness);
        }

        normalize(betweenness, N);

        std::vector<size_t> order(N);
        std::iota(order.begin(), order.end(), 0);
        std::partial_sort(order.begin(),order.begin()+std::min(top_k,N),order.end(),
                  [&](size_t a,size_t b) { return betweenness[a]!=betweenness[b] ? betweenness[a]>betweenness[b] : a<b; });

        const size_t show = std::min(top_k, N);
        double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();

        return AlgoResult{true, "", buildJson(snapshot, nodes, order, betweenness, show), ms};
    }

    void BetweennessCentrality::forwardBfs(
            const StaticGraph&                    snapshot,
            const std::vector<DenseId>&           nodes,
            size_t                                s_idx,
            BfsState&                             st)
    {
        size_t head=0;
        while (head<st.queue.size()) {
            snapshot.checkCancelled(); size_t v=st.queue[head++];

            auto expand = [&](DenseId nbr_id) -> bool {
                const size_t w=nbr_id;
                if (st.seen[w]==v) return true;
                st.seen[w]=v;

                if (st.dist[w] < 0) {
                    st.dist[w] = st.dist[v] + 1;
                    st.queue.push_back(w);
                }
                if (st.dist[w] == st.dist[v] + 1) {
                    st.sigma[w] += st.sigma[v];
                    st.pred_storage[st.pred_offsets[w]+st.pred_counts[w]++]=v;
                }
                return true;
            };

            for (const auto& edge:snapshot.outgoing(nodes[v])) { snapshot.checkCancelled(); expand(edge.neighbor); }
            for (const auto& edge:snapshot.incoming(nodes[v])) { snapshot.checkCancelled(); expand(edge.neighbor); }
        }
    }
    void BetweennessCentrality::backwardPass(size_t     s_idx,
    BfsState&      st,
            std::vector<double>& betweenness)
{
    auto& delta=st.delta;
    for (auto it=st.queue.rbegin();it!=st.queue.rend();++it) {
    const auto w=*it;
    for (size_t p=0;p<st.pred_counts[w];++p) {
    const auto v=st.pred_storage[st.pred_offsets[w]+p];
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

AlgoResult runBetweennessCentrality(GraphManager& manager,const std::string& graph_name,
        const std::vector<ExtId>& params,SnapshotOptions options) {
    auto algorithm=std::make_shared<BetweennessCentrality>();
    auto handle=manager.submitJob(graph_name,algorithm,params,std::move(options));
    return handle.result();
}

AlgoResult runBetweennessCentrality(GraphManager& manager,
                                    const std::string& graph_name,
                                    const std::vector<ExtId>& params)
{
    auto algo=std::make_shared<BetweennessCentrality>();
    auto handle = manager.submitJob(graph_name, algo, params);
    return handle.result();
}

}
