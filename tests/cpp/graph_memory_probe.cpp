#include "graph/Livegraph.h"
#include <chrono>
#include <iostream>

using namespace nexora::graph;
namespace {
struct Counts { static inline size_t current=0,peak=0,allocations=0; };
template<class T> struct CountAllocator {
    using value_type=T;
    CountAllocator()=default;
    template<class U> CountAllocator(const CountAllocator<U>&) {}
    T* allocate(size_t n) {
        auto* result=std::allocator<T>{}.allocate(n);
        Counts::current+=n*sizeof(T); Counts::peak=std::max(Counts::peak,Counts::current); ++Counts::allocations;
        return result;
    }
    void deallocate(T* p,size_t n) { Counts::current-=n*sizeof(T); std::allocator<T>{}.deallocate(p,n); }
    template<class U> bool operator==(const CountAllocator<U>&) const { return true; }
};
template<class K,class V> using CountMap=std::unordered_map<K,V,std::hash<K>,std::equal_to<K>,CountAllocator<std::pair<const K,V>>>;
// Evaluation-only linear-probing flat map. Not SwissTable and not production:
// deliberately no erase, concurrency, adversarial-hash or exception-safety claims.
class FlatProbe {
    struct Slot { std::string key; DenseId value=0; bool occupied=false; };
    std::vector<Slot> slots_;
public:
    explicit FlatProbe(size_t count) {
        size_t size=1; while (double(count)/size>0.7) size*=2; slots_.resize(size);
    }
    void insert(const std::string& key,DenseId value) {
        size_t i=std::hash<std::string>{}(key)&(slots_.size()-1);
        while (slots_[i].occupied && slots_[i].key!=key) i=(i+1)&(slots_.size()-1);
        slots_[i]={key,value,true};
    }
    DenseId get(const std::string& key) const {
        size_t i=std::hash<std::string>{}(key)&(slots_.size()-1);
        while (slots_[i].occupied) {
            if (slots_[i].key==key) return slots_[i].value;
            i=(i+1)&(slots_.size()-1);
        }
        return kInvalidDenseId;
    }
    size_t bytes() const { return slots_.capacity()*sizeof(Slot); }
};
template<class Fn> double nsPerLookup(const std::vector<std::string>& keys,Fn fn) {
    uint64_t sum=0; const auto start=std::chrono::steady_clock::now();
    for (int repeat=0;repeat<20;++repeat) for (const auto& key:keys) sum+=fn(key);
    const auto ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();
    std::cout<<"checksum="<<sum<<" "; return ns/(20*keys.size());
}
}
int main() {
    struct LegacyNodeAdj { bool out_heavy=false,in_heavy=false; std::vector<AdjEntry> out,in; ChunkedSortedVector heavy_out,heavy_in; };
    std::cout<<"sizeof NodeRecord="<<sizeof(NodeRecord)<<" EdgeRecord="<<sizeof(EdgeRecord)
        <<" AdjEntry="<<sizeof(AdjEntry)<<" NodeAdj.old="<<sizeof(LegacyNodeAdj)
        <<" NodeAdj.new="<<sizeof(NodeAdj)<<" optional_string="<<sizeof(std::optional<ExtId>)<<"\n";
    constexpr size_t count=10000;
    {
        Counts::current=Counts::peak=Counts::allocations=0;
        CountMap<EdgeId,EdgeRecord> old;
        for (size_t i=0;i<count;++i) { EdgeRecord edge{}; edge.edge_id=i; edge.flags=FLAG_ACTIVE; old[i]=edge; }
        const auto old_bytes=Counts::current;
        EdgeStore store; for (const auto& [id,edge]:old) store[id]=edge;
        store.optimizeDense();
        std::cout<<"edges="<<count<<" hash.requested_bytes="<<old_bytes
            <<" dense.buffer_bytes="<<store.slotCapacity()*sizeof(EdgeRecord)
            <<" sparse_records="<<store.sparseSize()<<"\n";
    }
    for (size_t nodes:{1000U,10000U,100000U}) {
        std::vector<std::string> keys; keys.reserve(nodes);
        for (size_t i=0;i<nodes;++i) keys.push_back("n"+std::to_string(i));
        Counts::current=Counts::peak=Counts::allocations=0;
        CountMap<std::string,DenseId> standard;
        for (size_t i=0;i<nodes;++i) standard.emplace(keys[i],i);
        FlatProbe flat(nodes); for (size_t i=0;i<nodes;++i) flat.insert(keys[i],i);
        std::cout<<"ids="<<nodes<<" hash.requested_bytes="<<Counts::current
            <<" hash.allocations="<<Counts::allocations<<" flat.buffer_bytes="<<flat.bytes()<<" ";
        const auto standard_ns=nsPerLookup(keys,[&](const auto& key) { return standard.at(key); });
        const auto flat_ns=nsPerLookup(keys,[&](const auto& key) { return flat.get(key); });
        std::cout<<"hash.lookup_ns="<<standard_ns<<" flat.lookup_ns="<<flat_ns<<"\n";
    }
    LiveGraph graph(nullptr,nullptr,"probe");
    for (size_t i=0;i<count;++i) graph.addNode("n"+std::to_string(i),"Node");
    const auto isolated=graph.memoryUsage();
    for (size_t i=0;i<count;i+=2) graph.addEdge("n"+std::to_string(i),"n"+std::to_string(i+1),"link");
    const auto pairs=graph.memoryUsage();
    std::cout<<"nodes="<<count<<" isolated.total_estimate="<<isolated.total_bytes_estimate
        <<" isolated.adj_payload="<<isolated.adjacency_payload_bytes
        <<" pairs.total_estimate="<<pairs.total_bytes_estimate<<" pairs.adj_payload="<<pairs.adjacency_payload_bytes
        <<" pair_edges="<<graph.activeEdgeCount()<<"\n";
}
