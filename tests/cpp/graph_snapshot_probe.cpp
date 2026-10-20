#include "graph/BulkGraphBuilder.h"
#include "graph/StaticGraph.h"
#include <chrono>
#include <future>
#include <iostream>
#include <queue>

using namespace nexora::graph;
template<class Fn> double ms(Fn fn) {
    const auto start=std::chrono::steady_clock::now(); fn();
    return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
}
struct BfsBuffers {
    std::vector<int> distance;
    std::vector<DenseId> queue;
    explicit BfsBuffers(size_t n):distance(n) { queue.reserve(n); }
    size_t run(const StaticGraph& snapshot,size_t source) {
        std::fill(distance.begin(),distance.end(),-1); queue.clear(); distance[source]=0; queue.push_back(source);
        for (size_t head=0;head<queue.size();++head) {
            const auto current=queue[head];
            for (const auto& edge:snapshot.outgoing(current)) if (distance[edge.neighbor]<0) {
                distance[edge.neighbor]=distance[current]+1; queue.push_back(edge.neighbor);
            }
        }
        return queue.size();
    }
};
int main() {
    for (size_t n:{1000U,10000U}) {
        LiveGraph graph(nullptr,nullptr,"probe"); BulkGraphBuilder builder(graph);
        for (size_t id=0;id<n;++id) for (size_t step=1;step<=4;++step)
            builder.addEdge(std::to_string(id),std::to_string((id+step)%n),"link");
        builder.flush(); std::unique_ptr<StaticGraph> snapshot;
        SnapshotOptions options; options.max_build_bytes=64*1024*1024;
        const auto build_ms=ms([&] { snapshot=std::make_unique<StaticGraph>(graph,std::vector<std::string>{},std::vector<std::string>{},"probe",options); });
        size_t legacy_bytes=n*sizeof(std::vector<AdjEntry>);
        const auto legacy_staging_ms=ms([&] {
            std::vector<std::vector<AdjEntry>> lists(n);
            for (size_t id=0;id<n;++id) {
                const auto row=snapshot->outgoing(id); lists[id].assign(row.begin(),row.end());
                legacy_bytes+=lists[id].capacity()*sizeof(AdjEntry);
            }
        });
        const auto csr_bytes=snapshot->compressedBytes();
        const auto csc_ms=ms([&] { (void)snapshot->incoming(0); });
        double repeat_ms=ms([&] { for (int i=0;i<5;++i) { StaticGraph fresh(graph); (void)fresh.outDegree(0); } });
        const auto clones_ms=ms([&] { for (int i=0;i<5;++i) { auto clone=snapshot->clone(); (void)clone->outDegree(0); } });
        std::cout<<"n="<<n<<" csr_build_ms="<<build_ms<<" csr_bytes="<<csr_bytes
            <<" legacy_adj_bytes="<<legacy_bytes<<" legacy_adj_staging_ms="<<legacy_staging_ms
            <<" lazy_csc_ms="<<csc_ms<<" csr_csc_bytes="<<snapshot->compressedBytes()
            <<" allocation_peak_bytes="<<snapshot->allocationPeakBytes()
            <<" allocation_budget="<<options.max_build_bytes
            <<" allocation_current_bytes="<<snapshot->allocationCurrentBytes()
            <<" planned_peak_bound="<<snapshot->plannedPeakBytes()<<" five_rebuilds_ms="<<repeat_ms
            <<" five_cached_clones_ms="<<clones_ms<<"\n";
        for (int repeat=0;repeat<3;++repeat) {
            size_t baseline_checksum=0,serial_checksum=0,parallel_checksum=0;
            const auto baseline=ms([&] {
                for (size_t source=0;source<32;++source) {
                    std::unordered_map<DenseId,int> distance; std::queue<DenseId> queue;
                    distance[source]=0; queue.push(source);
                    while (!queue.empty()) {
                        const auto current=queue.front(); queue.pop();
                        for (const auto& edge:snapshot->outgoing(current)) if (!distance.count(edge.neighbor)) {
                            distance[edge.neighbor]=distance[current]+1; queue.push(edge.neighbor);
                        }
                    }
                    baseline_checksum+=distance.size();
                }
            });
            const auto serial=ms([&] { BfsBuffers buffers(n); for (size_t i=0;i<32;++i) serial_checksum+=buffers.run(*snapshot,i); });
            const auto parallel=ms([&] {
                auto worker=[&](size_t first) { BfsBuffers buffers(n); size_t sum=0;
                    for (size_t i=first;i<32;i+=2) sum+=buffers.run(*snapshot,i);
                    return sum; };
                auto future=std::async(std::launch::async,worker,0);
                parallel_checksum=worker(1)+future.get();
            });
            if (baseline_checksum!=serial_checksum || serial_checksum!=parallel_checksum) return 2;
            std::cout<<"n="<<n<<" run="<<repeat<<" hash_bfs_ms="<<baseline<<" reused_bfs_ms="<<serial
                <<" two_workers_ms="<<parallel<<" checksum="<<serial_checksum<<"\n";
        }
    }
}
