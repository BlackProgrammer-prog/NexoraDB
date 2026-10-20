#include "graph/BulkGraphBuilder.h"
#include <atomic>
#include <barrier>
#include <chrono>
#include <iostream>
#include <thread>
using namespace nexora::graph;
int main() {
    constexpr size_t per_writer=5000;
    for (bool hub:{false,true}) for (size_t writers:{1U,2U,4U}) {
        LiveGraph graph(nullptr,nullptr,"locking_probe"); BulkGraphBuilder builder(graph);
        for (size_t i=0;i<2*per_writer*writers+1;++i) builder.addNode(std::to_string(i),"Node");
        builder.flush(); std::barrier start(static_cast<ptrdiff_t>(writers+1));
        std::atomic<size_t> completed=0; std::vector<std::thread> workers;
        for (size_t worker=0;worker<writers;++worker) workers.emplace_back([&,worker] {
            start.arrive_and_wait();
            for (size_t i=0;i<per_writer;++i) {
                const auto id=worker*per_writer+i;
                graph.addEdge(hub ? "0" : std::to_string(2*id+1),std::to_string(2*id+2),"link");
                ++completed;
            }
        });
        const auto begin=std::chrono::steady_clock::now(); start.arrive_and_wait();
        for (auto& worker:workers) worker.join();
        const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
        if (graph.activeEdgeCount()!=writers*per_writer) return 1;
        std::cout<<"hub="<<hub<<" writers="<<writers<<" edges="<<completed
            <<" seconds="<<seconds<<" edges_per_second="<<completed/seconds<<"\n";
    }
}
