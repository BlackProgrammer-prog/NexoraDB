// Benchmark-only allocation instrumentation. Never linked into the engine.
#include "graph/Livegraph.h"
#include <cstdlib>
#include <iostream>
#include <new>
#include <sys/resource.h>
using namespace nexora::graph;
namespace {
bool counting=false;
size_t live=0,peak=0;
struct Header { void* raw; size_t bytes; bool counted; };
void* allocate(size_t n,size_t alignment) {
    alignment=std::max(alignment,alignof(Header));
    if (n>SIZE_MAX-alignment-sizeof(Header)) throw std::bad_alloc();
    void* raw=std::malloc(n+alignment+sizeof(Header)); if (!raw) throw std::bad_alloc();
    auto address=(reinterpret_cast<uintptr_t>(raw)+sizeof(Header)+alignment-1)&~(alignment-1);
    auto* h=reinterpret_cast<Header*>(address)-1; *h={raw,n,counting};
    if (counting) { live+=n; peak=std::max(peak,live); }
    return reinterpret_cast<void*>(address);
}
void release(void* p) noexcept {
    if (!p) return;
    auto* h=static_cast<Header*>(p)-1;
    if (h->counted) live-=h->bytes;
    std::free(h->raw);
}
struct LegacyAdj {
    bool out_heavy=false,in_heavy=false;
    std::vector<AdjEntry> out,in;
    ChunkedSortedVector heavy_out,heavy_in;
};
struct Legacy {
    std::vector<NodeRecord> nodes;
    std::vector<LegacyAdj> adjacency;
    std::unordered_map<std::string,DenseId> forward;
    std::unordered_map<DenseId,std::string> reverse;
    std::unordered_map<EdgeId,EdgeRecord> edges;
};
void report(const char* mode,size_t nodes,size_t isolated,size_t paired,size_t root) {
    counting=false; rusage usage{}; getrusage(RUSAGE_SELF,&usage);
    std::cout<<"mode="<<mode<<" nodes="<<nodes<<" edges="<<nodes/2
        <<" isolated_requested="<<isolated+root<<" paired_requested="<<paired+root
        <<" requested_per_node="<<double(isolated+root)/nodes
        <<" marginal_requested_per_edge="<<double(paired-isolated)/(nodes/2)
        <<" requested_peak="<<peak+root<<" maxrss_kib="<<usage.ru_maxrss<<"\n";
}
}
void* operator new(size_t n) { return allocate(n,alignof(std::max_align_t)); }
void* operator new[](size_t n) { return ::operator new(n); }
void* operator new(size_t n,std::align_val_t a) { return allocate(n,size_t(a)); }
void* operator new[](size_t n,std::align_val_t a) { return ::operator new(n,a); }
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p,size_t) noexcept { release(p); }
void operator delete[](void* p,size_t) noexcept { release(p); }
void operator delete(void* p,std::align_val_t) noexcept { release(p); }
void operator delete[](void* p,std::align_val_t) noexcept { release(p); }
void operator delete(void* p,size_t,std::align_val_t) noexcept { release(p); }
void operator delete[](void* p,size_t,std::align_val_t) noexcept { release(p); }
int main(int argc,char** argv) {
    const bool legacy=argc>1 && std::string_view(argv[1])=="legacy";
    constexpr size_t n=100000; counting=true;
    if (legacy) {
        Legacy graph;
        for (size_t i=0;i<n;++i) {
            const auto ext="n"+std::to_string(i); graph.nodes.emplace_back(); graph.adjacency.emplace_back();
            graph.forward.emplace(ext,i); graph.reverse.emplace(i,ext);
        }
        const auto isolated=live;
        for (size_t i=0;i<n;i+=2) {
            EdgeRecord edge{}; edge.edge_id=i/2; edge.src=i; edge.dst=i+1; edge.flags=FLAG_ACTIVE;
            graph.edges.emplace(edge.edge_id,edge);
            graph.adjacency[i].out.push_back({i+1,edge.edge_id,0});
            graph.adjacency[i+1].in.push_back({i,edge.edge_id,0});
        }
        report("legacy-layout-fixture",n,isolated,live,sizeof(graph));
    } else {
        LiveGraph graph(nullptr,nullptr,"probe");
        for (size_t i=0;i<n;++i) graph.addNode("n"+std::to_string(i),"Node");
        const auto isolated=live;
        for (size_t i=0;i<n;i+=2) graph.addEdge("n"+std::to_string(i),"n"+std::to_string(i+1),"link");
        report("live-dense",n,isolated,live,sizeof(graph));
    }
}
