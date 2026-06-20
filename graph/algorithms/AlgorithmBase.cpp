//
// Created by HOME on 6/19/2026.
//

#include "AlgorithmBase.h"
/**
 * @file graph/algorithms/AlgorithmBase.cpp
 * @brief پیاده‌سازی StaticGraphView
 */


#include <algorithm>

namespace nexora {
    namespace graph {
        namespace algorithms {

// ══════════════════════════════════════════════════════════════
// StaticGraphView — ساخت snapshot از LiveGraph
// ══════════════════════════════════════════════════════════════

            StaticGraphView::StaticGraphView(const LiveGraph& source) {
                snapshot_version_ = source.version();

                // کپی nodes
                source.forEachNode([&](DenseId id, const NodeRecord& rec) -> bool {
                    if (id >= nodes_.size()) nodes_.resize(id + 1);
                    nodes_[id] = {rec.type_id, rec.out_degree, rec.in_degree,
                                  source.getExtId(id)};
                    ++node_count_;
                    return true;
                });

                // کپی edges و adjacency
                source.forEachEdge([&](EdgeId eid, const EdgeRecord& rec) -> bool {
                    edges_[eid] = {rec.src, rec.dst, rec.type_id};

                    AdjEntry fwd{rec.dst, eid, rec.type_id};
                    out_adj_[rec.src].push_back(fwd);

                    AdjEntry rev{rec.src, eid, rec.type_id};
                    in_adj_[rec.dst].push_back(rev);

                    ++edge_count_;
                    return true;
                });

                // کپی type names از طریق LiveGraph
                // (در نسخه MVP از یک range استفاده می‌کنیم)
                source.forEachNode([&](DenseId id, const NodeRecord& rec) -> bool {
                    if (type_names_.find(rec.type_id) == type_names_.end())
                        type_names_[rec.type_id] = source.getNodeTypeName(rec.type_id);
                    return true;
                });
            }

            std::vector<DenseId> StaticGraphView::neighbors(DenseId   dense_id,
                                                            Direction direction,
                                                            TypeId    type_id) const {
                std::vector<DenseId> result;

                auto collect = [&](const std::unordered_map<DenseId,
                        std::vector<AdjEntry>>& adj_map) {
                    auto it = adj_map.find(dense_id);
                    if (it == adj_map.end()) return;
                    for (const auto& e : it->second) {
                        if (type_id != kInvalidTypeId && e.type_id != type_id) continue;
                        if (e.neighbor < nodes_.size()) result.push_back(e.neighbor);
                    }
                };

                if (direction == Direction::Out || direction == Direction::Both)
                    collect(out_adj_);
                if (direction == Direction::In || direction == Direction::Both)
                    collect(in_adj_);

                return result;
            }

            void StaticGraphView::forEachNode(
                    const std::function<bool(DenseId, TypeId)>& fn) const {
                for (size_t i = 0; i < nodes_.size(); ++i) {
                    const auto& n = nodes_[i];
                    if (n.type_id == kInvalidTypeId && n.ext_id.empty()) continue;
                    if (!fn(static_cast<DenseId>(i), n.type_id)) break;
                }
            }

            void StaticGraphView::forEachEdge(
                    const std::function<bool(EdgeId, DenseId, DenseId, TypeId)>& fn) const {
                for (const auto& [eid, e] : edges_)
                    if (!fn(eid, e.src, e.dst, e.type_id)) break;
            }

            uint64_t StaticGraphView::outDegree(DenseId id) const {
                if (id >= nodes_.size()) return 0;
                return nodes_[id].out_degree;
            }

            uint64_t StaticGraphView::inDegree(DenseId id) const {
                if (id >= nodes_.size()) return 0;
                return nodes_[id].in_degree;
            }

            TypeId StaticGraphView::nodeType(DenseId id) const {
                if (id >= nodes_.size()) return kInvalidTypeId;
                return nodes_[id].type_id;
            }

            std::string StaticGraphView::typeName(TypeId type_id) const {
                auto it = type_names_.find(type_id);
                return (it != type_names_.end()) ? it->second : "";
            }

            ExtId StaticGraphView::extId(DenseId id) const {
                if (id >= nodes_.size()) return "";
                return nodes_[id].ext_id;
            }

            bool StaticGraphView::hasNode(DenseId id) const {
                if (id >= nodes_.size()) return false;
                return !nodes_[id].ext_id.empty() || nodes_[id].type_id != kInvalidTypeId;
            }

            bool StaticGraphView::hasEdge(DenseId src, DenseId dst, TypeId type_id) const {
                auto it = out_adj_.find(src);
                if (it == out_adj_.end()) return false;
                for (const auto& e : it->second) {
                    if (e.neighbor != dst) continue;
                    if (type_id == kInvalidTypeId || e.type_id == type_id) return true;
                }
                return false;
            }

        } // namespace algorithms
    } // namespace graph
} // namespace nexora