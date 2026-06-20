//
// Created by HOME on 6/19/2026.
//

/**
 * @file graph/StaticGraph.cpp
 * @brief پیاده‌سازی کامل StaticGraph
 */

#include "StaticGraph.h"
#include "Livegraph.h"

#include <algorithm>
#include <stdexcept>

namespace nexora {
    namespace graph {

// ══════════════════════════════════════════════════════════════
// §1  سازنده‌ها
// ══════════════════════════════════════════════════════════════

        StaticGraph::StaticGraph(const LiveGraph& source, std::string name)
                : name_(std::move(name))
        {
            buildFromLiveGraph(source, {}, {});
        }

        StaticGraph::StaticGraph(const LiveGraph&                source,
                                 const std::vector<std::string>& node_types,
                                 const std::vector<std::string>& edge_types,
                                 std::string                     name)
                : name_(std::move(name))
        {
            buildFromLiveGraph(source, node_types, edge_types);
        }

// ══════════════════════════════════════════════════════════════
// §2  buildFromLiveGraph — قلب snapshot
// ══════════════════════════════════════════════════════════════

        void StaticGraph::buildFromLiveGraph(
                const LiveGraph&                source,
                const std::vector<std::string>& filter_node_types,
                const std::vector<std::string>& filter_edge_types)
        {
            snapshot_version_ = source.version();

            // ── ساخت فیلتر TypeId ──
            std::unordered_map<TypeId, bool> allowed_nt;
            std::unordered_map<TypeId, bool> allowed_et;
            const bool do_filter_nodes = !filter_node_types.empty();
            const bool do_filter_edges = !filter_edge_types.empty();

            if (do_filter_nodes) {
                for (const auto& name : filter_node_types) {
                    auto tid = source.getNodeTypeId(name);
                    if (tid) allowed_nt[*tid] = true;
                }
            }
            if (do_filter_edges) {
                for (const auto& name : filter_edge_types) {
                    auto tid = source.getEdgeTypeId(name);
                    if (tid) allowed_et[*tid] = true;
                }
            }

            // ── کپی nodes ──
            source.forEachNode([&](DenseId id, const NodeRecord& rec) -> bool {
                if (do_filter_nodes && !allowed_nt.count(rec.type_id))
                    return true;  // skip

                // resize در صورت نیاز
                if (id >= nodes_.size()) {
                    nodes_.resize(id + 1);
                    out_adj_.resize(id + 1);
                    in_adj_.resize(id + 1);
                }

                ExtId ext = source.getExtId(id);
                nodes_[id].type_id    = rec.type_id;
                nodes_[id].out_degree = rec.out_degree;
                nodes_[id].in_degree  = rec.in_degree;
                nodes_[id].ext_id     = ext;
                nodes_[id].valid      = true;

                if (!ext.empty()) ext_to_dense_[ext] = id;

                // ثبت نام type
                if (!node_type_names_.count(rec.type_id))
                    node_type_names_[rec.type_id] = source.getNodeTypeName(rec.type_id);

                ++node_count_;
                return true;
            });

            // ── کپی edges ──
            source.forEachEdge([&](EdgeId eid, const EdgeRecord& er) -> bool {
                if (do_filter_edges && !allowed_et.count(er.type_id))
                    return true;

                // اگر src یا dst در snapshot نیست → skip
                if (er.src >= nodes_.size() || !nodes_[er.src].valid) return true;
                if (er.dst >= nodes_.size() || !nodes_[er.dst].valid) return true;

                edges_[eid] = {er.src, er.dst, er.type_id};

                // adjacency
                out_adj_[er.src].push_back({er.dst, eid, er.type_id});
                in_adj_[er.dst].push_back({er.src, eid, er.type_id});

                if (!edge_type_names_.count(er.type_id))
                    edge_type_names_[er.type_id] = source.getEdgeTypeName(er.type_id);

                ++edge_count_;
                return true;
            });
        }

// ══════════════════════════════════════════════════════════════
// §3  Node API
// ══════════════════════════════════════════════════════════════

        bool StaticGraph::hasNode(DenseId id) const noexcept {
            return id < nodes_.size() && nodes_[id].valid;
        }

        TypeId StaticGraph::nodeType(DenseId id) const noexcept {
            if (!hasNode(id)) return kInvalidTypeId;
            return nodes_[id].type_id;
        }

        uint64_t StaticGraph::outDegree(DenseId id) const noexcept {
            if (id >= out_adj_.size()) return 0;
            return static_cast<uint64_t>(out_adj_[id].size());
        }

        uint64_t StaticGraph::inDegree(DenseId id) const noexcept {
            if (id >= in_adj_.size()) return 0;
            return static_cast<uint64_t>(in_adj_[id].size());
        }

        ExtId StaticGraph::extId(DenseId id) const {
            if (!hasNode(id)) return "";
            return nodes_[id].ext_id;
        }

        DenseId StaticGraph::denseId(const ExtId& ext) const {
            auto it = ext_to_dense_.find(ext);
            return (it != ext_to_dense_.end()) ? it->second : kInvalidDenseId;
        }

        std::string StaticGraph::nodeTypeName(TypeId tid) const {
            auto it = node_type_names_.find(tid);
            return (it != node_type_names_.end()) ? it->second : "";
        }

        std::string StaticGraph::edgeTypeName(TypeId tid) const {
            auto it = edge_type_names_.find(tid);
            return (it != edge_type_names_.end()) ? it->second : "";
        }

// ══════════════════════════════════════════════════════════════
// §4  Traversal
// ══════════════════════════════════════════════════════════════

        std::vector<DenseId> StaticGraph::neighbors(DenseId   id,
                                                    Direction  direction,
                                                    TypeId     type_id) const {
            std::vector<DenseId> result;
            if (!hasNode(id)) return result;

            auto collect = [&](const std::vector<AdjEntry>& adj) {
                for (const auto& e : adj) {
                    if (type_id != kInvalidTypeId && e.type_id != type_id) continue;
                    if (hasNode(e.neighbor)) result.push_back(e.neighbor);
                }
            };

            if (direction == Direction::Out || direction == Direction::Both)
                collect(out_adj_[id]);
            if (direction == Direction::In || direction == Direction::Both)
                collect(in_adj_[id]);

            return result;
        }

        void StaticGraph::forEachNeighbor(
                DenseId   id,
                Direction  direction,
                const std::function<bool(DenseId, TypeId)>& fn) const
        {
            if (!hasNode(id)) return;

            auto iter = [&](const std::vector<AdjEntry>& adj) {
                for (const auto& e : adj) {
                    if (!hasNode(e.neighbor)) continue;
                    if (!fn(e.neighbor, e.type_id)) return;
                }
            };

            if (direction == Direction::Out || direction == Direction::Both)
                iter(out_adj_[id]);
            if (direction == Direction::In || direction == Direction::Both)
                iter(in_adj_[id]);
        }

        void StaticGraph::forEachNode(
                const std::function<bool(DenseId, TypeId)>& fn) const
        {
            for (size_t i = 0; i < nodes_.size(); ++i) {
                if (!nodes_[i].valid) continue;
                if (!fn(static_cast<DenseId>(i), nodes_[i].type_id)) break;
            }
        }

        void StaticGraph::forEachEdge(
                const std::function<bool(EdgeId, DenseId, DenseId, TypeId)>& fn) const
        {
            for (const auto& [eid, e] : edges_)
                if (!fn(eid, e.src, e.dst, e.type_id)) break;
        }

        bool StaticGraph::hasEdge(DenseId src, DenseId dst, TypeId type_id) const {
            if (src >= out_adj_.size()) return false;
            for (const auto& e : out_adj_[src]) {
                if (e.neighbor != dst) continue;
                if (type_id == kInvalidTypeId || e.type_id == type_id) return true;
            }
            return false;
        }

// ══════════════════════════════════════════════════════════════
// §5  Export COO / CSR
// ══════════════════════════════════════════════════════════════

        CooGraph StaticGraph::exportCOO(const GraphExportOptions& opts) const {
            CooGraph coo;

            // ساخت allowed sets
            std::unordered_map<TypeId, bool> allowed_nt, allowed_et;
            const bool fn = !opts.nodeTypes.empty();
            const bool fe = !opts.edgeTypes.empty();

            if (fn) for (const auto& n : opts.nodeTypes)
                    for (const auto& [tid, name] : node_type_names_)
                        if (name == n) allowed_nt[tid] = true;

            if (fe) for (const auto& e : opts.edgeTypes)
                    for (const auto& [tid, name] : edge_type_names_)
                        if (name == e) allowed_et[tid] = true;

            // remap nodes به 0..N-1
            std::unordered_map<DenseId, uint64_t> remap;
            if (opts.remapNodeIdsToContiguous) {
                uint64_t idx = 0;
                for (size_t i = 0; i < nodes_.size(); ++i) {
                    if (!nodes_[i].valid) continue;
                    if (fn && !allowed_nt.count(nodes_[i].type_id)) continue;
                    remap[static_cast<DenseId>(i)] = idx++;
                }
                coo.originalNodeIds.resize(remap.size());
                for (const auto& [orig, remapped] : remap)
                    coo.originalNodeIds[remapped] = orig;
            }

            // edges
            for (const auto& [eid, e] : edges_) {
                if (fe && !allowed_et.count(e.type_id)) continue;
                if (fn) {
                    if (e.src >= nodes_.size() || !allowed_nt.count(nodes_[e.src].type_id)) continue;
                    if (e.dst >= nodes_.size() || !allowed_nt.count(nodes_[e.dst].type_id)) continue;
                }

                uint64_t src_out, dst_out;
                if (opts.remapNodeIdsToContiguous) {
                    auto it_s = remap.find(e.src);
                    auto it_d = remap.find(e.dst);
                    if (it_s == remap.end() || it_d == remap.end()) continue;
                    src_out = it_s->second;
                    dst_out = it_d->second;
                } else {
                    src_out = e.src;
                    dst_out = e.dst;
                }

                coo.src.push_back(src_out);
                coo.dst.push_back(dst_out);
                coo.edgeTypeIds.push_back(e.type_id);
            }

            return coo;
        }

        CsrGraph StaticGraph::exportCSR(const GraphExportOptions& opts) const {
            // ابتدا COO، سپس تبدیل به CSR
            auto coo = exportCOO(opts);
            CsrGraph csr;
            csr.originalNodeIds = coo.originalNodeIds;
            csr.edgeTypeIds     = coo.edgeTypeIds;

            const uint64_t num_nodes = coo.originalNodeIds.empty()
                                       ? node_count_
                                       : static_cast<uint64_t>(coo.originalNodeIds.size());

            csr.rowPtr.assign(num_nodes + 1, 0);
            csr.colIdx.resize(coo.src.size());

            // شمارش out-degree هر node
            for (auto s : coo.src) {
                if (s < num_nodes) csr.rowPtr[s + 1]++;
            }

            // prefix sum
            for (uint64_t i = 1; i <= num_nodes; ++i)
                csr.rowPtr[i] += csr.rowPtr[i - 1];

            // fill colIdx
            std::vector<uint64_t> pos(csr.rowPtr.begin(), csr.rowPtr.end());
            for (size_t i = 0; i < coo.src.size(); ++i) {
                uint64_t s = coo.src[i];
                if (s < pos.size()) csr.colIdx[pos[s]++] = coo.dst[i];
            }

            return csr;
        }

// ══════════════════════════════════════════════════════════════
// §6  آمار — GraphStatsEx
// ══════════════════════════════════════════════════════════════

        GraphStatsEx StaticGraph::stats() const {
            GraphStatsEx s;
            // فیلدهای سازگار با GraphStats
            s.active_nodes  = node_count_;
            s.active_edges  = edge_count_;
            s.version       = snapshot_version_;
            // فیلدهای اضافه
            s.nodeCount     = node_count_;
            s.edgeCount     = edge_count_;
            s.nodeTypeCount = node_type_names_.size();
            s.edgeTypeCount = edge_type_names_.size();
            return s;
        }

        std::unordered_map<std::string, size_t> StaticGraph::nodeCountByType() const {
            std::unordered_map<std::string, size_t> result;
            for (const auto& n : nodes_) {
                if (!n.valid) continue;
                result[nodeTypeName(n.type_id)]++;
            }
            return result;
        }

        std::unordered_map<std::string, size_t> StaticGraph::edgeCountByType() const {
            std::unordered_map<std::string, size_t> result;
            for (const auto& [eid, e] : edges_)
                result[edgeTypeName(e.type_id)]++;
            return result;
        }

    } // namespace graph
} // namespace nexora