#include "StaticGraph.h"
#include "Livegraph.h"
#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace nexora::graph {
std::optional<std::array<uint64_t,5>> StaticGraph::edgeAt(size_t offset) const {
    checkCancelled(); if (offset>=data_->edges_.size()) return std::nullopt;
    const auto& [id,edge]=data_->edges_[offset];
    return std::array<uint64_t,5>{id,edge.src,edge.dst,edge.type_id,uint64_t(!edge.undirected)};
}
StaticGraph::StaticGraph(const LiveGraph& source,std::string name)
    :StaticGraph(source,{},{},std::move(name),{}) {}
StaticGraph::StaticGraph(const LiveGraph& source,const std::vector<std::string>& nt,
                         const std::vector<std::string>& et,std::string name)
    :StaticGraph(source,nt,et,std::move(name),{}) {}
StaticGraph::StaticGraph(const LiveGraph& source,const std::vector<std::string>& nt,
                         const std::vector<std::string>& et,std::string name,SnapshotOptions options)
    :name_(std::move(name)),options_(std::move(options)) {
    if (!options_.cancellation) options_.cancellation=std::make_shared<std::atomic<bool>>(false);
    data_=std::make_unique<Data>(options_.max_build_bytes);
    buildFromLiveGraph(source,nt,et);
}
void StaticGraph::checkCancelled() const {
    if (options_.deadline!=std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now()>=options_.deadline)
        throw std::runtime_error("snapshot/job timeout");
    if (options_.cancellation && options_.cancellation->load(std::memory_order_relaxed))
        throw std::runtime_error("snapshot/job cancelled");
}
void StaticGraph::cancel() const { options_.cancellation->store(true,std::memory_order_relaxed); }
std::unique_ptr<StaticGraph> StaticGraph::clone(SnapshotOptions options) const {
    return std::unique_ptr<StaticGraph>(new StaticGraph(*this,std::move(options)));
}
StaticGraph::StaticGraph(const StaticGraph& source,SnapshotOptions options)
    :name_(source.name_),options_(std::move(options)),planned_peak_bytes_(source.planned_peak_bytes_),
     node_count_(source.node_count_),edge_count_(source.edge_count_),snapshot_version_(source.snapshot_version_) {
    if (!options_.cancellation) options_.cancellation=std::make_shared<std::atomic<bool>>(false);
    checkCancelled();
    if (planned_peak_bytes_>SIZE_MAX/2 || (options_.max_build_bytes && planned_peak_bytes_*2>options_.max_build_bytes))
        throw std::length_error("snapshot cache/clone peak budget exceeded");
    planned_peak_bytes_*=2;
    data_=std::make_unique<Data>(options_.max_build_bytes,source.data_->memory.currentBytes());
    data_->nodes_.reserve(source.data_->nodes_.size());
    for (const auto& node:source.data_->nodes_) {
        checkCancelled(); data_->nodes_.emplace_back(node.type_id,node.ext_id,&data_->memory);
        data_->nodes_.back().out_degree=node.out_degree; data_->nodes_.back().in_degree=node.in_degree;
    }
    data_->edges_=source.data_->edges_; checkCancelled();
    data_->out_rows_=source.data_->out_rows_; data_->out_entries_=source.data_->out_entries_; data_->original_ids_=source.data_->original_ids_;
    checkCancelled(); data_->node_type_names_=source.data_->node_type_names_; data_->edge_type_names_=source.data_->edge_type_names_;
    data_->ext_to_dense_=source.data_->ext_to_dense_; checkCancelled();
    if (options_.incoming) ensureIncoming();
}
void StaticGraph::buildFromLiveGraph(const LiveGraph& source,const std::vector<std::string>& nt,
                                    const std::vector<std::string>& et) {
    checkCancelled(); std::shared_lock lock(source.mutex_);
    if (source.bulk_failed_) throw std::runtime_error("cannot snapshot failed graph writer");
    snapshot_version_=source.version(); size_t plan=sizeof(*this);
    const size_t limit=options_.max_build_bytes ? options_.max_build_bytes : SIZE_MAX;
    auto charge=[&](size_t count,size_t unit) {
        if (plan>limit || count>(limit-plan)/unit) throw std::length_error("snapshot build memory budget exceeded");
        plan+=count*unit;
    };
    charge(source.node_records_.size(),sizeof(DenseId));
    charge(source.active_node_count_,1024); charge(source.edge_records_.size(),256);
    for (const auto& node:source.node_records_) if (node.isActive()) {
        checkCancelled();
        if (const auto* ext=source.id_map_.findExt(node.dense_id)) charge(ext->size(),4);
    }
    for (const auto* registry:{&source.node_type_reg_,&source.edge_type_reg_})
        for (const auto& [id,name]:registry->id_to_name) { checkCancelled(); charge(name.size()+1,4); }
    // Include lazy CSC headroom as well: the same budget applies at first use.
    charge(source.edge_records_.size(),64);
    planned_peak_bytes_=plan;
    const auto allowed=[](TypeId id,const TypeRegistry& registry,const std::vector<std::string>& names) {
        if (names.empty()) return true;
        const auto found=registry.id_to_name.find(id);
        return found!=registry.id_to_name.end() && std::find(names.begin(),names.end(),found->second)!=names.end();
    };
    std::pmr::vector<DenseId> remap(source.node_records_.size(),kInvalidDenseId,&data_->memory);
    data_->nodes_.reserve(source.active_node_count_); data_->original_ids_.reserve(source.active_node_count_);
    for (const auto& rec:source.node_records_) {
        checkCancelled();
        if (!rec.isActive() || !allowed(rec.type_id,source.node_type_reg_,nt)) continue;
        const DenseId id=data_->nodes_.size(); remap[rec.dense_id]=id;
        const auto* identity=source.id_map_.findExt(rec.dense_id);
        const std::string_view ext=identity ? std::string_view(*identity) : std::string_view{};
        data_->nodes_.emplace_back(rec.type_id,ext,&data_->memory); data_->original_ids_.push_back(rec.dense_id);
        if (identity) data_->ext_to_dense_[std::pmr::string(ext,&data_->memory)]=id;
        if (!data_->node_type_names_.count(rec.type_id)) {
            const auto found=source.node_type_reg_.id_to_name.find(rec.type_id);
            data_->node_type_names_.try_emplace(rec.type_id,found==source.node_type_reg_.id_to_name.end() ? std::string_view{} : std::string_view(found->second));
        }
    }
    node_count_=data_->nodes_.size(); data_->edges_.reserve(source.edge_records_.size());
    for (const auto& [eid,edge]:source.edge_records_) {
        checkCancelled();
        if (!allowed(edge.type_id,source.edge_type_reg_,et)) continue;
        if (edge.src>=remap.size() || edge.dst>=remap.size() ||
            remap[edge.src]==kInvalidDenseId || remap[edge.dst]==kInvalidDenseId) continue;
        const auto src=remap[edge.src],dst=remap[edge.dst]; const bool both=edge.flags&FLAG_UNDIRECTED;
        data_->edges_.push_back({eid,{src,dst,edge.type_id,both}});
        ++data_->nodes_[src].out_degree; ++data_->nodes_[dst].in_degree;
        if (both && src!=dst) { ++data_->nodes_[dst].out_degree; ++data_->nodes_[src].in_degree; }
        if (!data_->edge_type_names_.count(edge.type_id)) {
            const auto found=source.edge_type_reg_.id_to_name.find(edge.type_id);
            data_->edge_type_names_.try_emplace(edge.type_id,found==source.edge_type_reg_.id_to_name.end() ? std::string_view{} : std::string_view(found->second));
        }
    }
    edge_count_=data_->edges_.size(); data_->out_rows_.assign(node_count_+1,0);
    for (size_t i=0;i<data_->nodes_.size();++i) data_->out_rows_[i+1]=data_->out_rows_[i]+data_->nodes_[i].out_degree;
    data_->out_entries_.resize(data_->out_rows_.back());
    { std::pmr::vector<uint64_t> cursor(data_->out_rows_,&data_->memory);
      for (const auto& [eid,edge]:data_->edges_) {
        checkCancelled(); data_->out_entries_[cursor[edge.src]++]={edge.dst,eid,edge.type_id};
        if (edge.undirected && edge.src!=edge.dst) data_->out_entries_[cursor[edge.dst]++]={edge.src,eid,edge.type_id};
      }
    }
    for (size_t id=0;id<data_->nodes_.size();++id) {
        checkCancelled(); std::sort(data_->out_entries_.begin()+data_->out_rows_[id],data_->out_entries_.begin()+data_->out_rows_[id+1]);
    }
    lock.unlock(); std::pmr::vector<DenseId>(&data_->memory).swap(remap);
    if (options_.incoming) ensureIncoming();
}
void StaticGraph::ensureIncoming() const {
    std::lock_guard lock(*incoming_mutex_); checkCancelled(); if (!data_->in_rows_.empty()) return;
    std::pmr::vector<uint64_t> rows(node_count_+1,0,&data_->memory);
    for (size_t i=0;i<data_->nodes_.size();++i) rows[i+1]=rows[i]+data_->nodes_[i].in_degree;
    std::pmr::vector<AdjEntry> entries(rows.back(),&data_->memory);
    std::pmr::vector<uint64_t> cursor(rows,&data_->memory);
    for (const auto& [eid,edge]:data_->edges_) {
        checkCancelled(); entries[cursor[edge.dst]++]={edge.src,eid,edge.type_id};
        if (edge.undirected && edge.src!=edge.dst) entries[cursor[edge.src]++]={edge.dst,eid,edge.type_id};
    }
    for (size_t id=0;id<data_->nodes_.size();++id) {
        checkCancelled(); std::sort(entries.begin()+rows[id],entries.begin()+rows[id+1]);
    }
    data_->in_rows_=std::move(rows); data_->in_entries_=std::move(entries);
}
bool StaticGraph::hasIncomingIndex() const { std::lock_guard lock(*incoming_mutex_); return !data_->in_rows_.empty(); }
size_t StaticGraph::compressedBytes() const {
    std::lock_guard lock(*incoming_mutex_);
    return (data_->out_rows_.capacity()+data_->in_rows_.capacity())*sizeof(uint64_t)+
           (data_->out_entries_.capacity()+data_->in_entries_.capacity())*sizeof(AdjEntry);
}
std::span<const AdjEntry> StaticGraph::outgoing(DenseId id) const {
    checkCancelled(); if (!hasNode(id)) return {};
    return std::span<const AdjEntry>(data_->out_entries_).subspan(data_->out_rows_[id],data_->out_rows_[id+1]-data_->out_rows_[id]);
}
std::span<const AdjEntry> StaticGraph::incoming(DenseId id) const {
    checkCancelled(); if (!hasNode(id)) return {}; ensureIncoming();
    return std::span<const AdjEntry>(data_->in_entries_).subspan(data_->in_rows_[id],data_->in_rows_[id+1]-data_->in_rows_[id]);
}
bool StaticGraph::hasNode(DenseId id) const noexcept { return id<data_->nodes_.size(); }
TypeId StaticGraph::nodeType(DenseId id) const noexcept { return hasNode(id) ? data_->nodes_[id].type_id : kInvalidTypeId; }
uint64_t StaticGraph::outDegree(DenseId id) const noexcept { return hasNode(id) ? data_->nodes_[id].out_degree : 0; }
uint64_t StaticGraph::inDegree(DenseId id) const noexcept { return hasNode(id) ? data_->nodes_[id].in_degree : 0; }
ExtId StaticGraph::extId(DenseId id) const { return hasNode(id) ? std::string(data_->nodes_[id].ext_id) : ""; }
DenseId StaticGraph::denseId(const ExtId& ext) const {
    auto found=data_->ext_to_dense_.find(ext); return found==data_->ext_to_dense_.end() ? kInvalidDenseId : found->second;
}
std::string StaticGraph::nodeTypeName(TypeId id) const { auto it=data_->node_type_names_.find(id); return it==data_->node_type_names_.end() ? std::string{} : std::string(it->second); }
std::string StaticGraph::edgeTypeName(TypeId id) const { auto it=data_->edge_type_names_.find(id); return it==data_->edge_type_names_.end() ? std::string{} : std::string(it->second); }
std::vector<DenseId> StaticGraph::neighbors(DenseId id,Direction direction,TypeId type) const {
    std::vector<DenseId> result;
    forEachNeighbor(id,direction,[&](DenseId neighbor,TypeId t) { if (type==kInvalidTypeId || t==type) result.push_back(neighbor); return true; });
    return result;
}
void StaticGraph::forEachNeighbor(DenseId id,Direction direction,const std::function<bool(DenseId,TypeId)>& fn) const {
    auto visit=[&](auto entries) { for (const auto& e:entries) { checkCancelled(); if (!fn(e.neighbor,e.type_id)) return false; } return true; };
    if (direction!=Direction::In && !visit(outgoing(id))) return;
    if (direction!=Direction::Out) visit(incoming(id));
}
void StaticGraph::forEachNode(const std::function<bool(DenseId,TypeId)>& fn) const {
    for (size_t id=0;id<data_->nodes_.size();++id) { checkCancelled(); if (!fn(id,data_->nodes_[id].type_id)) break; }
}
void StaticGraph::forEachEdge(const std::function<bool(EdgeId,DenseId,DenseId,TypeId)>& fn) const {
    for (const auto& [id,edge]:data_->edges_) { checkCancelled(); if (!fn(id,edge.src,edge.dst,edge.type_id)) break; }
}
bool StaticGraph::hasEdge(DenseId src,DenseId dst,TypeId type) const {
    for (const auto& edge:outgoing(src)) if (edge.neighbor==dst && (type==kInvalidTypeId || edge.type_id==type)) return true;
    return false;
}
CooGraph StaticGraph::exportCOO(const GraphExportOptions& options) const {
    CooGraph result; std::vector<uint64_t> remap(node_count_,kInvalidDenseId);
    for (size_t id=0;id<data_->nodes_.size();++id) {
        checkCancelled(); const auto& names=options.nodeTypes;
        if (!names.empty() && std::find(names.begin(),names.end(),nodeTypeName(data_->nodes_[id].type_id))==names.end()) continue;
        remap[id]=options.remapNodeIdsToContiguous ? result.originalNodeIds.size() : data_->original_ids_[id];
        if (options.remapNodeIdsToContiguous) result.originalNodeIds.push_back(data_->original_ids_[id]);
    }
    for (const auto& [id,edge]:data_->edges_) {
        checkCancelled(); const auto& names=options.edgeTypes;
        if (remap[edge.src]==kInvalidDenseId || remap[edge.dst]==kInvalidDenseId ||
            (!names.empty() && std::find(names.begin(),names.end(),edgeTypeName(edge.type_id))==names.end())) continue;
        result.src.push_back(remap[edge.src]); result.dst.push_back(remap[edge.dst]); result.edgeTypeIds.push_back(edge.type_id);
    }
    return result;
}
CsrGraph StaticGraph::exportCSR(const GraphExportOptions& options) const {
    CsrGraph result; std::vector<uint64_t> remap(node_count_,kInvalidDenseId); size_t count=0;
    for (size_t id=0;id<data_->nodes_.size();++id) {
        checkCancelled(); const auto& names=options.nodeTypes;
        if (!names.empty() && std::find(names.begin(),names.end(),nodeTypeName(data_->nodes_[id].type_id))==names.end()) continue;
        remap[id]=options.remapNodeIdsToContiguous ? count++ : data_->original_ids_[id];
        if (options.remapNodeIdsToContiguous) result.originalNodeIds.push_back(data_->original_ids_[id]);
        else count=std::max(count,size_t(data_->original_ids_[id]+1));
    }
    result.rowPtr.assign(count+1,0);
    const auto allowed=[&](const AdjEntry& e) {
        return remap[e.neighbor]!=kInvalidDenseId && (options.edgeTypes.empty() ||
            std::find(options.edgeTypes.begin(),options.edgeTypes.end(),edgeTypeName(e.type_id))!=options.edgeTypes.end());
    };
    for (size_t id=0;id<data_->nodes_.size();++id) if (remap[id]!=kInvalidDenseId) {
        for (const auto& e:outgoing(id)) { checkCancelled(); if (allowed(e)) ++result.rowPtr[remap[id]+1]; }
    }
    std::partial_sum(result.rowPtr.begin(),result.rowPtr.end(),result.rowPtr.begin());
    result.colIdx.resize(result.rowPtr.back()); result.edgeTypeIds.resize(result.rowPtr.back()); auto cursor=result.rowPtr;
    for (size_t id=0;id<data_->nodes_.size();++id) if (remap[id]!=kInvalidDenseId) {
        for (const auto& e:outgoing(id)) if (allowed(e)) {
            checkCancelled(); const auto at=cursor[remap[id]]++; result.colIdx[at]=remap[e.neighbor]; result.edgeTypeIds[at]=e.type_id;
        }
    }
    return result;
}
GraphStatsEx StaticGraph::stats() const {
    GraphStatsEx result; result.active_nodes=result.nodeCount=node_count_;
    result.active_edges=result.edgeCount=edge_count_; result.version=snapshot_version_;
    result.nodeTypeCount=data_->node_type_names_.size(); result.edgeTypeCount=data_->edge_type_names_.size(); return result;
}
std::unordered_map<std::string,size_t> StaticGraph::nodeCountByType() const {
    std::unordered_map<std::string,size_t> result; forEachNode([&](auto,auto type) { ++result[nodeTypeName(type)]; return true; }); return result;
}
std::unordered_map<std::string,size_t> StaticGraph::edgeCountByType() const {
    std::unordered_map<std::string,size_t> result; forEachEdge([&](auto,auto,auto,auto type) { ++result[edgeTypeName(type)]; return true; }); return result;
}
}
