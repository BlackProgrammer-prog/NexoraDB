#include "BulkGraphBuilder.h"
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

namespace nexora::graph {
BulkGraphBuilder::BulkGraphBuilder(LiveGraph& graph,BulkOptions options)
    :graph_(graph),options_(options) {
    if (!options.max_records || options.max_buffer_bytes<1024)
        throw std::invalid_argument("invalid bulk ingestion limits");
}
void BulkGraphBuilder::admit(size_t bytes) {
    if (failed_) throw std::runtime_error("bulk builder failed; reopen graph before retry");
    // Conservative logical charge includes input copies and per-record workspaces.
    // Resident graph, allocator overhead, and storage metadata are not this budget.
    if (bytes>(options_.max_buffer_bytes-1024)/4)
        throw std::length_error("one bulk record exceeds ingestion buffer budget");
    const size_t charge=1024+4*bytes;
    if (nodes_.size()+edges_.size()>=options_.max_records ||
        charge>options_.max_buffer_bytes-buffered_bytes_) flush();
    buffered_bytes_+=charge;
    stats_.peak_buffer_bytes=std::max(stats_.peak_buffer_bytes,buffered_bytes_);
}
void BulkGraphBuilder::addNode(const ExtId& id,const std::string& type) {
    if (id.empty()) throw std::invalid_argument("empty bulk node identity");
    admit(id.size()+type.size()); nodes_.push_back({id,type});
}
void BulkGraphBuilder::addEdge(const ExtId& src,const ExtId& dst,const std::string& type,bool directed) {
    if (src.empty() || dst.empty()) throw std::invalid_argument("empty bulk edge endpoint");
    admit(src.size()+dst.size()+type.size()); edges_.push_back({src,dst,type,directed});
}
void BulkGraphBuilder::flush() {
    if (failed_) throw std::runtime_error("bulk builder failed; reopen graph before retry");
    if (nodes_.empty() && edges_.empty()) return;
    std::unique_lock lock(graph_.mutex_); // Exactly one graph lock per batch.
    auto& g=graph_;
    if (g.bulk_failed_) throw std::runtime_error("graph requires reopen after failed bulk ingestion");
    size_t text=0;
    for (const auto& node:nodes_) text+=node.id.size()+node.type.size();
    for (const auto& edge:edges_) text+=edge.src.size()+edge.dst.size()+edge.type.size();
    g.admitMemory(nodes_.size()+2*edges_.size(),edges_.size(),text);
    try {
        std::vector<WalRecord> redo;
        std::vector<GraphStorage::Identity> identities;
        std::map<TypeId,std::string> edge_types;
        std::set<DenseId> touched;
        std::vector<EdgeRecord> new_edges;
        auto log=[&](WalOpType op,uint64_t id,uint64_t src,uint64_t dst,TypeId type,uint32_t flags) {
            WalRecord rec{}; rec.op=op; rec.node_or_edge_id=id; rec.src_id=src;
            rec.dst_id=dst; rec.type_id=type; rec.flags=flags; redo.push_back(rec);
        };
        auto node=[&](const ExtId& ext,const std::string& type,bool implicit) {
            auto id=g.id_map_.get(ext);
            if (id!=kInvalidDenseId && (implicit || !g.node_records_[id].isImplicit())) return id;
            const bool created=id==kInvalidDenseId;
            if (created) { id=g.allocDenseId(); g.ensureCapacity(id); g.id_map_.getOrCreate(ext,id); }
            auto& rec=g.node_records_[id];
            if (created) rec={};
            rec.dense_id=id; rec.type_id=implicit ? kInvalidTypeId : g.node_type_reg_.getOrCreate(type);
            rec.flags=FLAG_ACTIVE | (implicit ? uint32_t(FLAG_IMPLICIT) : 0U);
            identities.push_back({id,ext,rec.type_id,type}); touched.insert(id);
            log(WalOpType::AddNode,id,0,0,rec.type_id,rec.flags);
            if (created) { ++stats_.nodes_created; ++g.active_node_count_; }
            return id;
        };
        // Explicit nodes are resolved before implicit endpoints, irrespective of input order.
        for (const auto& input:nodes_) node(input.id,input.type,false);
        struct Candidate { DenseId src,dst; TypeId type; bool directed; size_t ordinal=0; bool exists=false; };
        std::vector<Candidate> candidates; candidates.reserve(edges_.size());
        for (const auto& input:edges_) {
            auto src=node(input.src,{},true),dst=node(input.dst,{},true);
            const auto type=g.edge_type_reg_.getOrCreate(input.type); edge_types[type]=input.type;
            if (!input.directed && dst<src) std::swap(src,dst);
            candidates.push_back({src,dst,type,input.directed,candidates.size()});
        }
        auto key=[](const Candidate& e) { return std::tie(e.src,e.dst,e.type,e.directed); };
        std::sort(candidates.begin(),candidates.end(),[&](const auto& a,const auto& b) {
            return key(a)!=key(b) ? key(a)<key(b) : a.ordinal<b.ordinal;
        });
        const auto end=std::unique(candidates.begin(),candidates.end(),[&](const auto& a,const auto& b) { return key(a)==key(b); });
        stats_.edges_deduplicated+=std::distance(end,candidates.end()); candidates.erase(end,candidates.end());
        // Merge sorted candidate keys with existing sorted adjacency per source.
        // Adjacency identity is (neighbor,type), matching incremental CRUD.
        // Mixed-direction collisions cannot create parallel adjacency entries.
        for (auto first=candidates.begin();first!=candidates.end();) {
            auto last=std::find_if(first,candidates.end(),[&](const auto& e) { return e.src!=first->src; });
            g.adj_[first->src].forEachOut([&](const AdjEntry& adj) {
                const auto& edge=g.edge_records_.at(adj.edge_id);
                Candidate lookup{first->src,adj.neighbor,edge.type_id,false};
                auto match=std::lower_bound(first,last,lookup,[&](const auto& a,const auto& b) { return key(a)<key(b); });
                for (;match!=last && match->dst==lookup.dst && match->type==lookup.type;++match)
                    match->exists=true;
                return true;
            });
            first=last;
        }
        // Restore ingestion precedence after sorted dedup: mixed-direction
        // conflicts must have the same first-wins result as single inserts.
        std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b) { return a.ordinal<b.ordinal; });
        std::set<std::tuple<DenseId,DenseId,TypeId>> occupied;
        for (const auto& input:candidates) {
            bool exists=input.exists;
            if (!input.directed && !exists) {
                g.adj_[input.dst].forEachOut([&](const AdjEntry& e) {
                    if (e.neighbor==input.src && e.type_id==input.type) exists=true;
                    return !exists;
                });
            }
            if (exists || occupied.count({input.src,input.dst,input.type}) ||
                (!input.directed && occupied.count({input.dst,input.src,input.type}))) {
                ++stats_.edges_deduplicated; continue;
            }
            occupied.emplace(input.src,input.dst,input.type);
            if (!input.directed) occupied.emplace(input.dst,input.src,input.type);
            EdgeRecord edge{}; edge.edge_id=g.allocEdgeId(); edge.src=input.src; edge.dst=input.dst;
            edge.type_id=input.type; edge.flags=FLAG_ACTIVE | (input.directed ? 0U : uint32_t(FLAG_UNDIRECTED));
            new_edges.push_back(edge);
            log(WalOpType::AddEdge,edge.edge_id,edge.src,edge.dst,edge.type_id,edge.flags);
            ++g.node_records_[edge.src].out_degree; ++g.node_records_[edge.dst].in_degree;
            if (!input.directed && edge.src!=edge.dst) {
                ++g.node_records_[edge.dst].out_degree; ++g.node_records_[edge.src].in_degree;
            }
            touched.insert(edge.src); touched.insert(edge.dst);
        }
        if (redo.empty()) { nodes_.clear(); edges_.clear(); buffered_bytes_=0; return; }
        if (g.storage_ && !g.storage_->rememberBatch(identities,edge_types))
            throw std::runtime_error("bulk identity persistence failed: "+g.storage_->lastError());
        std::vector<uint64_t> sequences;
        if (g.wal_) {
            sequences=g.wal_->appendBatch(redo);
            if (sequences.size()!=redo.size()) throw std::runtime_error("bulk WAL append failed: "+g.wal_->lastError());
        }
        if (hook_) hook_(Point::WalDurable);
        // Reserve final degree before filling adjacency. Flatten heavy lists once;
        // bulk vectors stay sorted and can upgrade on later incremental mutations.
        for (const auto id:touched) {
            auto& adj=g.adj_[id]; const auto& rec=g.node_records_[id];
            if (adj.out_heavy) {
                std::vector<AdjEntry> flat; flat.reserve(rec.out_degree);
                adj.forEachOut([&](const AdjEntry& e) { flat.push_back(e); return true; });
                adj.heavy_out={}; adj.out_edges=std::move(flat); adj.out_heavy=false;
            } else adj.out_edges.reserve(rec.out_degree);
            if (adj.in_heavy) {
                std::vector<AdjEntry> flat; flat.reserve(rec.in_degree);
                adj.forEachIn([&](const AdjEntry& e) { flat.push_back(e); return true; });
                adj.heavy_in={}; adj.in_edges=std::move(flat); adj.in_heavy=false;
            } else adj.in_edges.reserve(rec.in_degree);
        }
        for (const auto& e:new_edges) {
            g.edge_records_[e.edge_id]=e;
            g.adj_[e.src].out_edges.push_back({e.dst,e.edge_id,e.type_id});
            g.adj_[e.dst].in_edges.push_back({e.src,e.edge_id,e.type_id});
            if ((e.flags&FLAG_UNDIRECTED) && e.src!=e.dst) {
                g.adj_[e.dst].out_edges.push_back({e.src,e.edge_id,e.type_id});
                g.adj_[e.src].in_edges.push_back({e.dst,e.edge_id,e.type_id});
            }
        }
        std::vector<NodeRecord> persisted; persisted.reserve(touched.size());
        for (const auto id:touched) {
            auto& adj=g.adj_[id];
            std::sort(adj.out_edges.begin(),adj.out_edges.end());
            std::sort(adj.in_edges.begin(),adj.in_edges.end());
            persisted.push_back(g.node_records_[id]);
        }
        if (g.storage_ && !g.storage_->writeBatch(persisted,new_edges))
            throw std::runtime_error("bulk graph data write failed: "+g.storage_->lastError());
        if (g.storage_ && (!g.wal_ || g.wal_->durability()!=WalDurability::Async) && !g.storage_->durableSync())
            throw std::runtime_error("bulk graph data sync failed");
        if (hook_) hook_(Point::DataWritten);
        if (g.wal_ && !g.wal_->markAppliedBatch(sequences)) throw std::runtime_error("bulk checkpoint failed");
        if (hook_) hook_(Point::Checkpointed);
        g.active_edge_count_+=new_edges.size(); ++g.version_;
        stats_.edges_created+=new_edges.size(); ++stats_.batches;
        nodes_.clear(); edges_.clear(); buffered_bytes_=0;
    } catch (...) { failed_=true; g.bulk_failed_=true; throw; }
}
}
