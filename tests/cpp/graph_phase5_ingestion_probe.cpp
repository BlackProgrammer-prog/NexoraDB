#include "TestTempDir.h"
#include "graph/BulkGraphBuilder.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace nexora::graph;
void run(size_t history,bool bulk,WalDurability policy,int repeat) {
    TestTempDir temp("phase5_ingestion_probe");
    GraphStorage storage(temp.path(),"probe");
    WalOptions options; options.durability=WalDurability::Async; options.segment_bytes=1024*1024*1024;
    GraphWAL prefill(temp.path()/"probe.wal",86400,options);
    if (!storage.open() || !prefill.open()) throw std::runtime_error("open");
    for (size_t i=0;i<history;++i) {
        const auto seq=prefill.append(WalOpType::Commit,0);
        if (!seq || !prefill.markApplied(seq)) throw std::runtime_error("prefill");
    }
    if (!prefill.checkpoint([&] { return storage.durableSync(); })) throw std::runtime_error("checkpoint");
    prefill.close(); options.durability=policy;
    GraphWAL wal(temp.path()/"probe.wal",86400,options);
    if (!wal.open()) throw std::runtime_error("reopen");
    LiveGraph graph(&storage,&wal,"probe");
    constexpr size_t nodes=200,edges=600;
    const auto start=std::chrono::steady_clock::now();
    if (bulk) {
        BulkOptions limits; limits.max_records=128; BulkGraphBuilder builder(graph,limits);
        for (size_t i=0;i<nodes;++i) builder.addNode(std::to_string(i),"Node");
        for (size_t i=0;i<nodes;++i) for (size_t j=1;j<=3;++j)
            builder.addEdge(std::to_string(i),std::to_string((i+j)%nodes),"link");
        builder.flush();
    } else {
        for (size_t i=0;i<nodes;++i) graph.addNode(std::to_string(i),"Node");
        for (size_t i=0;i<nodes;++i) for (size_t j=1;j<=3;++j)
            graph.addEdge(std::to_string(i),std::to_string((i+j)%nodes),"link");
    }
    const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    if (graph.activeNodeCount()!=nodes || graph.activeEdgeCount()!=edges) throw std::runtime_error("count mismatch");
    std::cout<<"repeat="<<repeat<<" history="<<history<<" bulk="<<bulk<<" sync="<<(policy==WalDurability::Sync)
        <<" ms="<<elapsed<<" records_per_sec="<<800000/elapsed<<" wal_delta="<<wal.totalEntries()-history<<"\n";
    if (!graph.checkpointWAL()) throw std::runtime_error("final checkpoint");
}
int main() {
    for (int repeat=0;repeat<3;++repeat) {
        for (size_t history:{1000U,100000U,1000000U})
            for (int i=0;i<2;++i) run(history,bool((i+repeat)%2),WalDurability::Async,repeat);
        for (int i=0;i<2;++i) run(0,bool((i+repeat)%2),WalDurability::Sync,repeat);
    }
}
