//
// Created by HOME on 6/19/2026.
//

#include "Livegraph.h"

/**
 * @file graph/LiveGraph.cpp
 * @brief پیاده‌سازی کامل LiveGraph
 */

#include <algorithm>
#include <cassert>
#include <chrono>
#include <stdexcept>

namespace nexora {
    namespace graph {

// ══════════════════════════════════════════════════════════════
// §0  ChunkedSortedVector پیاده‌سازی
// ══════════════════════════════════════════════════════════════

        void ChunkedSortedVector::insert(const AdjEntry& e) {
            if (chunks.empty()) {
                chunks.emplace_back();
            }

            // پیدا کردن chunk مناسب (binary search روی head‌ها)
            size_t target_chunk = chunks.size() - 1;
            for (size_t i = 0; i < chunks.size(); ++i) {
                if (chunks[i].empty() || e < chunks[i].front()) {
                    target_chunk = i;
                    break;
                }
                if (i + 1 < chunks.size() && e < chunks[i + 1].front()) {
                    target_chunk = i;
                    break;
                }
            }

            auto& chunk = chunks[target_chunk];
            // insert مرتب در chunk
            auto pos = std::lower_bound(chunk.begin(), chunk.end(), e);
            if (pos != chunk.end() && *pos == e) return;  // تکراری
            chunk.insert(pos, e);

            // split اگر chunk بیش از حد بزرگ شد
            if (chunk.size() > CHUNK * 2) {
                std::vector<AdjEntry> half(chunk.begin() + CHUNK, chunk.end());
                chunk.erase(chunk.begin() + CHUNK, chunk.end());
                chunks.insert(chunks.begin() + target_chunk + 1, std::move(half));
            }
        }

        bool ChunkedSortedVector::remove(DenseId neighbor, TypeId type_id) {
            for (auto& chunk : chunks) {
                AdjEntry key{neighbor, kInvalidEdgeId, type_id};
                auto it = std::lower_bound(chunk.begin(), chunk.end(), key);
                while (it != chunk.end() && it->neighbor == neighbor) {
                    if (it->type_id == type_id) {
                        chunk.erase(it);
                        return true;
                    }
                    ++it;
                }
            }
            return false;
        }

        bool ChunkedSortedVector::contains(DenseId neighbor, TypeId type_id) const {
            for (const auto& chunk : chunks) {
                AdjEntry key{neighbor, kInvalidEdgeId, type_id};
                auto it = std::lower_bound(chunk.begin(), chunk.end(), key);
                while (it != chunk.end() && it->neighbor == neighbor) {
                    if (it->type_id == type_id) return true;
                    ++it;
                }
            }
            return false;
        }

        size_t ChunkedSortedVector::size() const noexcept {
            size_t total = 0;
            for (const auto& c : chunks) total += c.size();
            return total;
        }

        void ChunkedSortedVector::forEach(
                const std::function<bool(const AdjEntry&)>& fn) const {
            for (const auto& chunk : chunks) {
                for (const auto& e : chunk) {
                    if (!fn(e)) return;
                }
            }
        }

// ══════════════════════════════════════════════════════════════
// §0.5  NodeAdj پیاده‌سازی
// ══════════════════════════════════════════════════════════════

        void NodeAdj::addOut(const AdjEntry& e) {
            if (!out_heavy) {
                auto pos = std::lower_bound(out_edges.begin(), out_edges.end(), e);
                if (pos != out_edges.end() && *pos == e) return;
                out_edges.insert(pos, e);
                checkAndUpgrade();
            } else {
                heavy_out.insert(e);
            }
        }

        void NodeAdj::addIn(const AdjEntry& e) {
            if (!in_heavy) {
                auto pos = std::lower_bound(in_edges.begin(), in_edges.end(), e);
                if (pos != in_edges.end() && *pos == e) return;
                in_edges.insert(pos, e);
                checkAndUpgrade();
            } else {
                heavy_in.insert(e);
            }
        }

        bool NodeAdj::removeOut(DenseId neighbor, TypeId type_id) {
            if (!out_heavy) {
                AdjEntry key{neighbor, kInvalidEdgeId, type_id};
                auto it = std::lower_bound(out_edges.begin(), out_edges.end(), key);
                while (it != out_edges.end() && it->neighbor == neighbor) {
                    if (it->type_id == type_id) { out_edges.erase(it); return true; }
                    ++it;
                }
                return false;
            }
            return heavy_out.remove(neighbor, type_id);
        }

        bool NodeAdj::removeIn(DenseId neighbor, TypeId type_id) {
            if (!in_heavy) {
                AdjEntry key{neighbor, kInvalidEdgeId, type_id};
                auto it = std::lower_bound(in_edges.begin(), in_edges.end(), key);
                while (it != in_edges.end() && it->neighbor == neighbor) {
                    if (it->type_id == type_id) { in_edges.erase(it); return true; }
                    ++it;
                }
                return false;
            }
            return heavy_in.remove(neighbor, type_id);
        }

        void NodeAdj::forEachOut(const std::function<bool(const AdjEntry&)>& fn) const {
            if (!out_heavy) {
                for (const auto& e : out_edges) {
                    if (!fn(e)) return;
                }
            }
            else heavy_out.forEach(fn);
        }

        void NodeAdj::forEachIn(const std::function<bool(const AdjEntry&)>& fn) const {
            if (!in_heavy) {
                for (const auto& e : in_edges) {
                    if (!fn(e)) return;
                }
            }
            else heavy_in.forEach(fn);
        }

        void NodeAdj::checkAndUpgrade() {
            if (!out_heavy && out_edges.size() > kHeavyNodeThreshold) {
                for (const auto& e : out_edges) heavy_out.insert(e);
                out_edges.clear();
                out_edges.shrink_to_fit();
                out_heavy = true;
            }
            if (!in_heavy && in_edges.size() > kHeavyNodeThreshold) {
                for (const auto& e : in_edges) heavy_in.insert(e);
                in_edges.clear();
                in_edges.shrink_to_fit();
                in_heavy = true;
            }
        }

// ══════════════════════════════════════════════════════════════
// §1  DenseIdMap
// ══════════════════════════════════════════════════════════════

        DenseId DenseIdMap::getOrCreate(const ExtId& ext_id, DenseId reuse_id) {
            auto it = ext_to_dense_.find(ext_id);
            if (it != ext_to_dense_.end()) return it->second;

            DenseId id = (reuse_id != kInvalidDenseId) ? reuse_id : next_dense_++;
            ext_to_dense_[ext_id] = id;
            dense_to_ext_[id]     = ext_id;
            if (reuse_id == kInvalidDenseId && id >= next_dense_) next_dense_ = id + 1;
            return id;
        }

        DenseId DenseIdMap::get(const ExtId& ext_id) const {
            auto it = ext_to_dense_.find(ext_id);
            return (it != ext_to_dense_.end()) ? it->second : kInvalidDenseId;
        }

        ExtId DenseIdMap::getExt(DenseId dense_id) const {
            auto it = dense_to_ext_.find(dense_id);
            return (it != dense_to_ext_.end()) ? it->second : "";
        }

        bool DenseIdMap::hasExt(const ExtId& ext_id) const {
            return ext_to_dense_.count(ext_id) > 0;
        }

        bool DenseIdMap::hasDense(DenseId dense_id) const {
            return dense_to_ext_.count(dense_id) > 0;
        }

        void DenseIdMap::remove(const ExtId& ext_id) {
            auto it = ext_to_dense_.find(ext_id);
            if (it == ext_to_dense_.end()) return;
            DenseId did = it->second;
            dense_to_ext_.erase(did);
            ext_to_dense_.erase(it);
        }

        void DenseIdMap::remove(DenseId dense_id) {
            auto it = dense_to_ext_.find(dense_id);
            if (it == dense_to_ext_.end()) return;
            ext_to_dense_.erase(it->second);
            dense_to_ext_.erase(it);
        }

// ══════════════════════════════════════════════════════════════
// §2  LiveGraph سازنده
// ══════════════════════════════════════════════════════════════

        LiveGraph::LiveGraph(GraphStorage* storage, GraphWAL* wal, std::string name)
                : name_(std::move(name)), storage_(storage), wal_(wal) {}

// ══════════════════════════════════════════════════════════════
// §3  Internal helpers
// ══════════════════════════════════════════════════════════════

        DenseId LiveGraph::allocDenseId() {
            if (!node_free_stack_.empty()) {
                DenseId id = node_free_stack_.top();
                node_free_stack_.pop();
                return id;
            }
            return next_dense_id_.fetch_add(1);
        }

        EdgeId LiveGraph::allocEdgeId() {
            if (!edge_free_stack_.empty()) {
                EdgeId id = edge_free_stack_.top();
                edge_free_stack_.pop();
                return id;
            }
            return next_edge_id_.fetch_add(1);
        }

        void LiveGraph::ensureCapacity(DenseId id) {
            // بدون lock — باید از داخل unique_lock فراخوانی شود
            if (id >= adj_.size()) {
                adj_.resize(id + 1);
                node_records_.resize(id + 1);
            }
        }

        void LiveGraph::persistNode(const NodeRecord& rec) {
            if (storage_ && !storage_->writeNode(rec))
                throw std::runtime_error("Graph node write failed");
        }

        void LiveGraph::persistEdge(const EdgeRecord& rec) {
            if (storage_ && !storage_->writeEdge(rec))
                throw std::runtime_error("Graph edge write failed");
        }

        uint64_t LiveGraph::appendWal(WalOpType op, uint64_t id, uint64_t src,
                                     uint64_t dst, uint32_t type, uint32_t flags) {
            if (bulk_failed_) throw std::runtime_error("bulk ingestion failed; reopen graph and replay WAL");
            if (!wal_) return UINT64_MAX;
            const auto seq = wal_->append(op, id, src, dst, type, flags);
            if (seq == UINT64_MAX) throw std::runtime_error("Graph WAL append failed: " + wal_->lastError());
            return seq;
        }

        void LiveGraph::finishWal(uint64_t seq) {
            if (!wal_) return;
            if (wal_->durability() != WalDurability::Async && storage_ && !storage_->durableSync())
                throw std::runtime_error("Graph data sync failed before WAL checkpoint");
            if (!wal_->markApplied(seq))
                throw std::runtime_error("Graph WAL checkpoint failed: " + wal_->lastError());
        }

// ══════════════════════════════════════════════════════════════
// §4  Startup: loadFromDisk
// ══════════════════════════════════════════════════════════════

        bool LiveGraph::loadFromDisk() {
            if (!storage_ || !storage_->isOpen()) return false;

            std::unique_lock lock(mutex_);
            adj_.clear();
            node_records_.clear();
            edge_records_.clear();
            id_map_ = DenseIdMap{};
            while (!node_free_stack_.empty()) node_free_stack_.pop();
            while (!edge_free_stack_.empty()) edge_free_stack_.pop();
            active_node_count_.store(0);
            active_edge_count_.store(0);

            // ── اسکن فایل .nex ──
            DenseId max_dense = 0;
            storage_->scanAllNodes([&](const NodeRecord& rec) -> bool {
                if (rec.dense_id >= adj_.size()) {
                    adj_.resize(rec.dense_id + 1);
                    node_records_.resize(rec.dense_id + 1);
                }
                node_records_[rec.dense_id] = rec;

                if (rec.isActive()) {
                    active_node_count_.fetch_add(1);
                    // ExtId and type names are restored below from this
                    // generation's checksummed metadata dictionary.
                } else if (rec.isDeleted()) {
                    node_free_stack_.push(rec.dense_id);
                }

                if (rec.dense_id > max_dense) max_dense = rec.dense_id;
                return true;
            });
            next_dense_id_.store(max_dense + 1);

            // Identity and type dictionaries are part of the same published
            // storage generation as node/edge records.
            node_type_reg_ = TypeRegistry{}; edge_type_reg_ = TypeRegistry{};
            for (const auto& [id,name] : storage_->nodeTypes()) {
                node_type_reg_.name_to_id[name]=id; node_type_reg_.id_to_name[id]=name;
                node_type_reg_.next_id=std::max(node_type_reg_.next_id,id+1);
            }
            for (const auto& [id,name] : storage_->edgeTypes()) {
                edge_type_reg_.name_to_id[name]=id; edge_type_reg_.id_to_name[id]=name;
                edge_type_reg_.next_id=std::max(edge_type_reg_.next_id,id+1);
            }
            for (const auto& [id,ext] : storage_->identities())
                if (id<node_records_.size() && node_records_[id].isActive()) id_map_.getOrCreate(ext,id);

            // ── اسکن فایل .nexr ──
            EdgeId max_edge = 0;
            storage_->scanAllEdges([&](const EdgeRecord& rec) -> bool {
                if (rec.isActive()) {
                    edge_records_[rec.edge_id] = rec;
                    active_edge_count_.fetch_add(1);

                    // adjacency بازسازی
                    if (rec.src < adj_.size() && rec.dst < adj_.size()) {
                        AdjEntry fwd{rec.dst, rec.edge_id, rec.type_id};
                        adj_[rec.src].addOut(fwd);
                        AdjEntry rev{rec.src, rec.edge_id, rec.type_id};
                        adj_[rec.dst].addIn(rev);
                        if (rec.flags & FLAG_UNDIRECTED) {
                            adj_[rec.dst].addOut({rec.src, rec.edge_id, rec.type_id});
                            adj_[rec.src].addIn({rec.dst, rec.edge_id, rec.type_id});
                        }
                    }
                } else if (rec.isDeleted()) {
                    edge_free_stack_.push(rec.edge_id);
                }

                if (rec.edge_id > max_edge) max_edge = rec.edge_id;
                return true;
            });
            next_edge_id_.store(max_edge + 1);

            version_.fetch_add(1);
            return true;
        }

        size_t LiveGraph::replayWAL() {
            if (!wal_) return 0;
            std::unique_lock lock(mutex_);
            size_t count = 0;
            for (const auto& rec : wal_->loadUnapplied()) {
                if (!applyWalRecord(rec)) throw std::runtime_error("Graph WAL replay failed");
                rebuildReplayState();
                finishWal(rec.seq);
                ++count;
            }
            return count;
        }

        bool LiveGraph::checkpointWAL() {
            std::unique_lock lock(mutex_);
            if (bulk_failed_) throw std::runtime_error("bulk ingestion failed; reopen graph");
            return wal_ && !wal_->hasPendingEntries() && wal_->checkpoint([&] {
                return !storage_ || storage_->durableSync();
            });
        }

        void LiveGraph::rebuildReplayState() {
            adj_.clear(); adj_.resize(node_records_.size());
            while (!node_free_stack_.empty()) node_free_stack_.pop();
            while (!edge_free_stack_.empty()) edge_free_stack_.pop();
            uint64_t nodes = 0;
            for (auto& node : node_records_) {
                node.in_degree = node.out_degree = 0;
                if (node.isActive()) ++nodes;
                else if (node.isDeleted()) node_free_stack_.push(node.dense_id);
            }
            for (const auto& [eid, edge] : edge_records_) {
                if (!edge.isActive() || edge.src >= adj_.size() || edge.dst >= adj_.size())
                    throw std::runtime_error("Invalid WAL edge endpoints");
                adj_[edge.src].addOut({edge.dst, eid, edge.type_id});
                adj_[edge.dst].addIn({edge.src, eid, edge.type_id});
                ++node_records_[edge.src].out_degree; ++node_records_[edge.dst].in_degree;
                if (edge.flags & FLAG_UNDIRECTED) {
                    adj_[edge.dst].addOut({edge.src, eid, edge.type_id});
                    adj_[edge.src].addIn({edge.dst, eid, edge.type_id});
                    if (edge.src!=edge.dst) {
                        ++node_records_[edge.dst].out_degree; ++node_records_[edge.src].in_degree;
                    }
                }
            }
            for (const auto& node : node_records_)
                if (node.isActive() || node.isDeleted()) persistNode(node);
            if (storage_) {
                id_map_=DenseIdMap{};
                for (const auto& [id,ext] : storage_->identities())
                    if (id<node_records_.size() && node_records_[id].isActive()) id_map_.getOrCreate(ext,id);
            }
            active_node_count_ = nodes; active_edge_count_ = edge_records_.size();
            next_dense_id_ = node_records_.size();
            uint64_t max_edge = next_edge_id_;
            for (const auto& [eid, edge] : edge_records_) max_edge = std::max(max_edge, eid+1);
            next_edge_id_ = max_edge;
        }

        bool LiveGraph::applyWalRecord(const WalRecord& rec) {
            switch (rec.op) {
                case WalOpType::AddNode: {
                    // در replay، ExtId موجود نیست — فقط DenseId و type_id
                    DenseId id = rec.node_or_edge_id;
                    ensureCapacity(id);
                    NodeRecord nr = node_records_[id];
                    nr.dense_id = id;
                    nr.type_id  = rec.type_id;
                    nr.flags    = rec.flags | FLAG_ACTIVE;
                    node_records_[id] = nr;
                    persistNode(nr);
                    return true;
                }
                case WalOpType::RemoveNode: {
                    DenseId id = rec.node_or_edge_id;
                    ensureCapacity(id);
                    // Removing a node is a single redo operation including its
                    // incident edges, even if a previous crash removed some.
                    for (auto it = edge_records_.begin(); it != edge_records_.end();) {
                        if (it->second.src == id || it->second.dst == id) {
                            auto deleted = it->second; deleted.flags = FLAG_DELETED;
                            persistEdge(deleted); it = edge_records_.erase(it);
                        } else ++it;
                    }
                    node_records_[id].dense_id = id;
                    node_records_[id].flags = FLAG_DELETED;
                    persistNode(node_records_[id]); id_map_.remove(id);
                    return true;
                }
                case WalOpType::AddEdge: {
                    EdgeId eid = rec.node_or_edge_id;
                    EdgeRecord er{};
                    er.edge_id = eid;
                    er.src     = rec.src_id;
                    er.dst     = rec.dst_id;
                    er.type_id = rec.type_id;
                    er.flags   = rec.flags | FLAG_ACTIVE;
                    edge_records_[eid] = er;
                    persistEdge(er);
                    return true;
                }
                case WalOpType::RemoveEdge: {
                    EdgeId eid = rec.node_or_edge_id;
                    auto it = edge_records_.find(eid);
                    EdgeRecord deleted{}; deleted.edge_id = eid;
                    deleted.src = rec.src_id; deleted.dst = rec.dst_id;
                    deleted.type_id = rec.type_id; deleted.flags = FLAG_DELETED;
                    persistEdge(deleted);
                    if (it != edge_records_.end()) edge_records_.erase(it);
                    return true;
                }
                default:
                    return true;  // Begin/Commit/Rollback — نادیده
            }
        }

// ══════════════════════════════════════════════════════════════
// §5  addNode
// ══════════════════════════════════════════════════════════════

        DenseId LiveGraph::addNode(const ExtId& ext_id,
                                   const std::string& type_name,
                                   bool is_implicit) {
            std::unique_lock lock(mutex_);
            if (bulk_failed_) throw std::runtime_error("bulk ingestion failed; reopen graph");

            // بررسی وجود
            DenseId existing = id_map_.get(ext_id);
            if (existing != kInvalidDenseId) {
                // اگر implicit بود و الان explicit می‌شود
                if (is_implicit == false &&
                    existing < node_records_.size() &&
                    node_records_[existing].isImplicit()) {
                    const auto type=node_type_reg_.getOrCreate(type_name);
                    if (storage_ && !storage_->rememberNode(existing,ext_id,type,type_name))
                        throw std::runtime_error("Graph promoted identity persistence failed");
                    const auto seq = appendWal(WalOpType::AddNode, existing, 0, 0,
                            type,
                            node_records_[existing].flags & ~FLAG_IMPLICIT);
                    node_records_[existing].flags &= ~FLAG_IMPLICIT;
                    node_records_[existing].type_id=type;
                    persistNode(node_records_[existing]);
                    finishWal(seq);
                }
                return existing;
            }

            TypeId type_id = node_type_reg_.getOrCreate(type_name);
            DenseId new_id  = allocDenseId();
            ensureCapacity(new_id);

            NodeRecord rec{};
            rec.dense_id  = new_id;
            rec.type_id   = type_id;
            rec.flags     = FLAG_ACTIVE | (is_implicit ? FLAG_IMPLICIT : 0u);
            rec.out_degree = 0;
            rec.in_degree  = 0;

            // Durable intent metadata precedes redo. Orphan identities from an
            // interrupted append are ignored while their node slot is inactive.
            if (storage_ && !storage_->rememberNode(new_id,ext_id,type_id,type_name))
                throw std::runtime_error("Graph identity persistence failed: "+storage_->lastError());

            // WAL
            uint64_t seq = kInvalidEdgeId;
            if (wal_) {
                seq = appendWal(WalOpType::AddNode, new_id, 0, 0, type_id, rec.flags);
            }

            node_records_[new_id] = rec;
            id_map_.getOrCreate(ext_id, new_id);

            // دیسک
            persistNode(rec);

            finishWal(seq);

            active_node_count_.fetch_add(1);
            version_.fetch_add(1);
            return new_id;
        }

// ══════════════════════════════════════════════════════════════
// §6  removeNode
// ══════════════════════════════════════════════════════════════

        bool LiveGraph::removeNode(const ExtId& ext_id) {
            DenseId id = getDenseId(ext_id);
            if (id == kInvalidDenseId) return false;
            return removeNode(id);
        }

        bool LiveGraph::removeNode(DenseId dense_id) {
            std::unique_lock lock(mutex_);
            if (bulk_failed_) throw std::runtime_error("bulk ingestion failed; reopen graph");

            if (dense_id >= node_records_.size()) return false;
            NodeRecord& rec = node_records_[dense_id];
            if (!rec.isActive()) return false;

            const auto seq = appendWal(WalOpType::RemoveNode, dense_id, 0, 0,
                                       rec.type_id, FLAG_DELETED);
            // حذف تمام edges مرتبط (بدون lock — قبلاً unique_lock داریم)
            removeAllEdgesOfNode(dense_id);

            // علامت‌گذاری
            rec.flags = FLAG_DELETED;

            // WAL
            // دیسک
            persistNode(rec);

            id_map_.remove(dense_id);
            node_free_stack_.push(dense_id);
            active_node_count_.fetch_sub(1);
            finishWal(seq);
            version_.fetch_add(1);
            return true;
        }

        void LiveGraph::removeAllEdgesOfNode(DenseId dense_id) {
            // جمع‌آوری edge_id‌های مرتبط
            std::vector<EdgeId> to_remove;
            adj_[dense_id].forEachOut([&](const AdjEntry& e) {
                to_remove.push_back(e.edge_id);
                return true;
            });
            adj_[dense_id].forEachIn([&](const AdjEntry& e) {
                to_remove.push_back(e.edge_id);
                return true;
            });

            for (EdgeId eid : to_remove) {
                auto it = edge_records_.find(eid);
                if (it == edge_records_.end()) continue;
                EdgeRecord& er = it->second;
                if (!er.isActive()) continue;

                const bool undirected=(er.flags&FLAG_UNDIRECTED) && er.src!=er.dst;
                er.flags = FLAG_DELETED;
                persistEdge(er);

                adj_[er.src].removeOut(er.dst, er.type_id);
                adj_[er.dst].removeIn(er.src, er.type_id);
                // The persisted tombstone has no direction flag, so remove
                // reverse entries too (a no-op for a directed edge).
                adj_[er.dst].removeOut(er.src, er.type_id);
                adj_[er.src].removeIn(er.dst, er.type_id);
                auto& src = node_records_[er.src];
                auto& dst = node_records_[er.dst];
                if (src.out_degree) --src.out_degree;
                if (dst.in_degree) --dst.in_degree;
                if (undirected) {
                    if (dst.out_degree) --dst.out_degree;
                    if (src.in_degree) --src.in_degree;
                }
                persistNode(src); persistNode(dst);

                edge_records_.erase(it);
                edge_free_stack_.push(eid);
                active_edge_count_.fetch_sub(1);
            }

            // پاک کردن adjacency خود node
            adj_[dense_id] = NodeAdj{};

            // به‌روزرسانی degree روی دیسک
            if (storage_ && !storage_->updateNodeDegree(dense_id, 0, 0))
                throw std::runtime_error("Graph degree write failed");
        }

// ══════════════════════════════════════════════════════════════
// §7  addEdge
// ══════════════════════════════════════════════════════════════

        EdgeId LiveGraph::addEdge(const ExtId& src_ext, const ExtId& dst_ext,
                                  const std::string& type_name, bool directed) {
            std::unique_lock lock(mutex_);
            if (bulk_failed_) throw std::runtime_error("bulk ingestion failed; reopen graph");

            TypeId edge_type = edge_type_reg_.getOrCreate(type_name);
            if (storage_ && !storage_->rememberEdgeType(edge_type,type_name))
                throw std::runtime_error("Graph edge type persistence failed: "+storage_->lastError());

            // پیدا کردن یا ساخت src
            DenseId src_id = id_map_.get(src_ext);
            if (src_id == kInvalidDenseId) {
                // implicit node — نیاز به unlock ندارد چون unique_lock داریم
                src_id = allocDenseId();
                ensureCapacity(src_id);
                NodeRecord nr{};
                nr.dense_id = src_id;
                nr.type_id  = kInvalidTypeId;
                nr.flags    = FLAG_ACTIVE | FLAG_IMPLICIT;
                if (storage_ && !storage_->rememberNode(src_id,src_ext,nr.type_id,{}))
                    throw std::runtime_error("Graph source identity persistence failed");
                const auto seq = appendWal(WalOpType::AddNode, src_id, 0, 0, nr.type_id, nr.flags);
                id_map_.getOrCreate(src_ext, src_id);
                node_records_[src_id] = nr;
                persistNode(nr);
                finishWal(seq);
                active_node_count_.fetch_add(1);
            }

            // پیدا کردن یا ساخت dst
            DenseId dst_id = id_map_.get(dst_ext);
            if (dst_id == kInvalidDenseId) {
                dst_id = allocDenseId();
                ensureCapacity(dst_id);
                NodeRecord nr{};
                nr.dense_id = dst_id;
                nr.type_id  = kInvalidTypeId;
                nr.flags    = FLAG_ACTIVE | FLAG_IMPLICIT;
                if (storage_ && !storage_->rememberNode(dst_id,dst_ext,nr.type_id,{}))
                    throw std::runtime_error("Graph target identity persistence failed");
                const auto seq = appendWal(WalOpType::AddNode, dst_id, 0, 0, nr.type_id, nr.flags);
                id_map_.getOrCreate(dst_ext, dst_id);
                node_records_[dst_id] = nr;
                persistNode(nr);
                finishWal(seq);
                active_node_count_.fetch_add(1);
            }

            // بررسی تکراری بودن edge
            bool exists = false;
            adj_[src_id].forEachOut([&](const AdjEntry& e) {
                if (e.neighbor == dst_id && e.type_id == edge_type) exists = true;
                return !exists;
            });
            if (exists) {
                // edge_id موجود را پیدا کن
                EdgeId found = kInvalidEdgeId;
                adj_[src_id].forEachOut([&](const AdjEntry& e) {
                    if (e.neighbor == dst_id && e.type_id == edge_type)
                        found = e.edge_id;
                    return found == kInvalidEdgeId;
                });
                return found;
            }

            EdgeId new_eid = allocEdgeId();

            EdgeRecord er{};
            er.edge_id = new_eid;
            er.src     = src_id;
            er.dst     = dst_id;
            er.type_id = edge_type;
            er.flags   = FLAG_ACTIVE | (directed ? 0U : FLAG_UNDIRECTED);
            const auto seq = appendWal(WalOpType::AddEdge, new_eid,
                                       src_id, dst_id, edge_type, er.flags);
            edge_records_[new_eid] = er;

            // Adjacency update
            AdjEntry fwd{dst_id, new_eid, edge_type};
            adj_[src_id].addOut(fwd);
            AdjEntry rev{src_id, new_eid, edge_type};
            adj_[dst_id].addIn(rev);

            if (!directed) {
                // undirected: هر دو طرف out هم هستند
                adj_[dst_id].addOut({src_id, new_eid, edge_type});
                adj_[src_id].addIn({dst_id, new_eid, edge_type});
            }

            // به‌روزرسانی degree در NodeRecord
            node_records_[src_id].out_degree++;
            node_records_[dst_id].in_degree++;
            if (!directed && src_id!=dst_id) {
                ++node_records_[dst_id].out_degree; ++node_records_[src_id].in_degree;
            }
            if (storage_) {
                if (!storage_->updateNodeDegree(src_id,
                                           node_records_[src_id].out_degree,
                                           node_records_[src_id].in_degree) ||
                    !storage_->updateNodeDegree(dst_id,
                                           node_records_[dst_id].out_degree,
                                           node_records_[dst_id].in_degree))
                    throw std::runtime_error("Graph degree write failed");
            }

            // WAL
            persistEdge(er);

            finishWal(seq);

            active_edge_count_.fetch_add(1);
            version_.fetch_add(1);
            return new_eid;
        }

// ══════════════════════════════════════════════════════════════
// §8  removeEdge
// ══════════════════════════════════════════════════════════════

        bool LiveGraph::removeEdge(EdgeId edge_id) {
            std::unique_lock lock(mutex_);
            if (bulk_failed_) throw std::runtime_error("bulk ingestion failed; reopen graph");

            auto it = edge_records_.find(edge_id);
            if (it == edge_records_.end()) return false;

            EdgeRecord& er = it->second;
            if (!er.isActive()) return false;

            const bool undirected=(er.flags&FLAG_UNDIRECTED) && er.src!=er.dst;

            const auto seq = appendWal(WalOpType::RemoveEdge, edge_id,
                                       er.src, er.dst, er.type_id, FLAG_DELETED);

            adj_[er.src].removeOut(er.dst, er.type_id);
            adj_[er.dst].removeIn(er.src, er.type_id);
            if (er.flags & FLAG_UNDIRECTED) {
                adj_[er.dst].removeOut(er.src, er.type_id);
                adj_[er.src].removeIn(er.dst, er.type_id);
            }

            er.flags = FLAG_DELETED;
            persistEdge(er);

            node_records_[er.src].out_degree =
                    (node_records_[er.src].out_degree > 0)
                    ? node_records_[er.src].out_degree - 1 : 0;
            node_records_[er.dst].in_degree =
                    (node_records_[er.dst].in_degree > 0)
                    ? node_records_[er.dst].in_degree - 1 : 0;
            if (undirected) {
                if (node_records_[er.dst].out_degree) --node_records_[er.dst].out_degree;
                if (node_records_[er.src].in_degree) --node_records_[er.src].in_degree;
            }

            if (storage_) {
                if (!storage_->updateNodeDegree(er.src,
                                           node_records_[er.src].out_degree,
                                           node_records_[er.src].in_degree) ||
                    !storage_->updateNodeDegree(er.dst,
                                           node_records_[er.dst].out_degree,
                                           node_records_[er.dst].in_degree))
                    throw std::runtime_error("Graph degree write failed");
            }

            edge_records_.erase(it);
            edge_free_stack_.push(edge_id);

            finishWal(seq);

            active_edge_count_.fetch_sub(1);
            version_.fetch_add(1);
            return true;
        }

        bool LiveGraph::removeEdge(const ExtId& src_ext, const ExtId& dst_ext,
                                   const std::string& type_name) {
            std::shared_lock rlock(mutex_);
            DenseId src_id = id_map_.get(src_ext);
            DenseId dst_id = id_map_.get(dst_ext);
            auto tid = edge_type_reg_.get(type_name);
            if (src_id == kInvalidDenseId || dst_id == kInvalidDenseId || !tid)
                return false;

            EdgeId found = kInvalidEdgeId;
            if (src_id < adj_.size()) {
                adj_[src_id].forEachOut([&](const AdjEntry& e) {
                    if (e.neighbor == dst_id && e.type_id == *tid)
                        found = e.edge_id;
                    return found == kInvalidEdgeId;
                });
            }
            rlock.unlock();

            if (found == kInvalidEdgeId) return false;
            return removeEdge(found);
        }

// ══════════════════════════════════════════════════════════════
// §9  Query helpers
// ══════════════════════════════════════════════════════════════

        bool LiveGraph::hasNode(const ExtId& ext_id) const {
            std::shared_lock lock(mutex_);
            return id_map_.hasExt(ext_id);
        }

        bool LiveGraph::hasNode(DenseId dense_id) const {
            std::shared_lock lock(mutex_);
            return dense_id < node_records_.size() && node_records_[dense_id].isActive();
        }

        std::optional<NodeView> LiveGraph::getNode(const ExtId& ext_id) const {
            std::shared_lock lock(mutex_);
            DenseId id = id_map_.get(ext_id);
            if (id == kInvalidDenseId) return std::nullopt;
            return getNode(id);
        }

        std::optional<NodeView> LiveGraph::getNode(DenseId dense_id) const {
            std::shared_lock lock(mutex_);
            if (dense_id >= node_records_.size()) return std::nullopt;
            const NodeRecord& rec = node_records_[dense_id];
            if (!rec.isActive()) return std::nullopt;

            NodeView v;
            v.dense_id    = dense_id;
            v.external_id = id_map_.getExt(dense_id);
            v.type_name   = node_type_reg_.getName(rec.type_id);
            v.flags       = rec.flags;
            v.out_degree  = rec.out_degree;
            v.in_degree   = rec.in_degree;
            return v;
        }

        DenseId LiveGraph::getDenseId(const ExtId& ext_id) const {
            std::shared_lock lock(mutex_);
            return id_map_.get(ext_id);
        }

        ExtId LiveGraph::getExtId(DenseId dense_id) const {
            std::shared_lock lock(mutex_);
            return id_map_.getExt(dense_id);
        }

        bool LiveGraph::hasEdge(const ExtId& src_ext, const ExtId& dst_ext,
                                const std::string& type_name) const {
            std::shared_lock lock(mutex_);
            DenseId src_id = id_map_.get(src_ext);
            DenseId dst_id = id_map_.get(dst_ext);
            auto tid = edge_type_reg_.get(type_name);
            if (src_id == kInvalidDenseId || dst_id == kInvalidDenseId || !tid)
                return false;
            if (src_id >= adj_.size()) return false;
            bool found = false;
            adj_[src_id].forEachOut([&](const AdjEntry& e) {
                if (e.neighbor == dst_id && e.type_id == *tid) found = true;
                return !found;
            });
            return found;
        }

        std::optional<EdgeView> LiveGraph::getEdge(EdgeId edge_id) const {
            std::shared_lock lock(mutex_);
            auto it = edge_records_.find(edge_id);
            if (it == edge_records_.end()) return std::nullopt;
            const EdgeRecord& er = it->second;
            if (!er.isActive()) return std::nullopt;

            EdgeView v;
            v.edge_id  = edge_id;
            v.src      = er.src;
            v.dst      = er.dst;
            v.src_ext  = id_map_.getExt(er.src);
            v.dst_ext  = id_map_.getExt(er.dst);
            v.type_name = edge_type_reg_.getName(er.type_id);
            v.flags    = er.flags;
            return v;
        }

// ══════════════════════════════════════════════════════════════
// §10  Traversal
// ══════════════════════════════════════════════════════════════

        std::vector<DenseId> LiveGraph::neighbors(DenseId dense_id,
                                                  Direction direction,
                                                  TypeId    type_id,
                                                  size_t    limit) const {
            std::shared_lock lock(mutex_);
            if (dense_id >= adj_.size()) return {};

            std::vector<DenseId> result;
            result.reserve(std::min(limit > 0 ? limit : size_t{64}, size_t{1024}));

            auto collect = [&](const std::function<void(
                    const std::function<bool(const AdjEntry&)>&)>& iter) {
                iter([&](const AdjEntry& e) {
                    if (limit > 0 && result.size() >= limit) return false;
                    if (type_id != kInvalidTypeId && e.type_id != type_id) return true;
                    DenseId nb = e.neighbor;
                    if (nb < node_records_.size() && node_records_[nb].isActive())
                        result.push_back(nb);
                    return limit == 0 || result.size() < limit;
                });
            };

            if (direction == Direction::Out || direction == Direction::Both)
                collect([&](auto fn) { adj_[dense_id].forEachOut(fn); });
            if (direction == Direction::In || direction == Direction::Both)
                collect([&](auto fn) { adj_[dense_id].forEachIn(fn); });

            return result;
        }

        std::vector<ExtId> LiveGraph::neighborsExt(const ExtId& ext_id,
                                                   Direction direction,
                                                   const std::string& type_name,
                                                   size_t limit) const {
            std::shared_lock lock(mutex_);
            DenseId dense_id = id_map_.get(ext_id);
            if (dense_id == kInvalidDenseId) return {};

            TypeId filter_type = kInvalidTypeId;
            if (!type_name.empty()) {
                auto tid = edge_type_reg_.get(type_name);
                if (!tid) return {};
                filter_type = *tid;
            }
            lock.unlock();

            auto dense_result = neighbors(dense_id, direction, filter_type, limit);

            std::shared_lock lock2(mutex_);
            std::vector<ExtId> result;
            result.reserve(dense_result.size());
            for (DenseId id : dense_result)
                result.push_back(id_map_.getExt(id));
            return result;
        }

        void LiveGraph::forEachNode(
                const std::function<bool(DenseId, const NodeRecord&)>& fn) const {
            std::shared_lock lock(mutex_);
            for (size_t i = 0; i < node_records_.size(); ++i) {
                if (node_records_[i].isActive()) {
                    if (!fn(static_cast<DenseId>(i), node_records_[i])) break;
                }
            }
        }

        void LiveGraph::forEachEdge(
                const std::function<bool(EdgeId, const EdgeRecord&)>& fn) const {
            std::shared_lock lock(mutex_);
            for (const auto& [eid, er] : edge_records_) {
                if (er.isActive()) {
                    if (!fn(eid, er)) break;
                }
            }
        }

        void LiveGraph::forEachOutEdge(
                DenseId dense_id,
                const std::function<bool(const AdjEntry&)>& fn) const {
            std::shared_lock lock(mutex_);
            if (dense_id >= adj_.size()) return;
            adj_[dense_id].forEachOut(fn);
        }

        void LiveGraph::forEachInEdge(
                DenseId dense_id,
                const std::function<bool(const AdjEntry&)>& fn) const {
            std::shared_lock lock(mutex_);
            if (dense_id >= adj_.size()) return;
            adj_[dense_id].forEachIn(fn);
        }

// ══════════════════════════════════════════════════════════════
// §11  Type Registry
// ══════════════════════════════════════════════════════════════

        TypeId LiveGraph::getOrCreateNodeType(const std::string& name) {
            std::unique_lock lock(mutex_);
            return node_type_reg_.getOrCreate(name);
        }
        TypeId LiveGraph::getOrCreateEdgeType(const std::string& name) {
            std::unique_lock lock(mutex_);
            return edge_type_reg_.getOrCreate(name);
        }
        std::optional<TypeId> LiveGraph::getNodeTypeId(const std::string& name) const {
            std::shared_lock lock(mutex_);
            return node_type_reg_.get(name);
        }
        std::optional<TypeId> LiveGraph::getEdgeTypeId(const std::string& name) const {
            std::shared_lock lock(mutex_);
            return edge_type_reg_.get(name);
        }
        std::string LiveGraph::getNodeTypeName(TypeId id) const {
            std::shared_lock lock(mutex_);
            return node_type_reg_.getName(id);
        }
        std::string LiveGraph::getEdgeTypeName(TypeId id) const {
            std::shared_lock lock(mutex_);
            return edge_type_reg_.getName(id);
        }

// ══════════════════════════════════════════════════════════════
// §12  Stats
// ══════════════════════════════════════════════════════════════

        GraphStats LiveGraph::stats() const {
            std::shared_lock lock(mutex_);
            GraphStats s;
            s.active_nodes   = active_node_count_.load();
            s.active_edges   = active_edge_count_.load();
            s.deleted_nodes  = node_free_stack_.size();
            s.deleted_edges  = edge_free_stack_.size();
            s.version        = version_.load();

            // heavy nodes شمارش
            for (size_t i = 0; i < adj_.size(); ++i) {
                if (adj_[i].out_heavy || adj_[i].in_heavy) s.heavy_nodes++;
            }
            return s;
        }

        uint64_t LiveGraph::activeNodeCount() const noexcept {
            return active_node_count_.load();
        }
        uint64_t LiveGraph::activeEdgeCount() const noexcept {
            return active_edge_count_.load();
        }

// ══════════════════════════════════════════════════════════════
// §13  Compaction
// ══════════════════════════════════════════════════════════════

        bool LiveGraph::compact() {
            if (!storage_) return false;

            std::unique_lock lock(mutex_);
            if (bulk_failed_) throw std::runtime_error("bulk ingestion failed; reopen graph");

            // No WAL containing old physical IDs may remain replayable across
            // compaction, including volatile Async applied markers.
            if (wal_ && (wal_->hasPendingEntries() ||
                !wal_->checkpoint([&] { return storage_->durableSync(); }))) return false;

            std::unordered_map<DenseId, DenseId> node_remap;
            std::unordered_map<EdgeId, EdgeId>   edge_remap;

            // Compact فایل‌های دیسک
            if (!storage_->compact(node_remap, edge_remap))
                throw std::runtime_error(storage_->lastError());

            // آپدیت RAM با remap
            // ── NodeRecords ──
            std::vector<NodeRecord> new_records;
            std::vector<NodeAdj>    new_adj;
            DenseIdMap              new_id_map;

            for (const auto& [old_id, new_id] : node_remap) {
                if (old_id >= node_records_.size()) continue;
                NodeRecord rec = node_records_[old_id];
                rec.dense_id = new_id;
                if (new_id >= new_records.size()) {
                    new_records.resize(new_id + 1);
                    new_adj.resize(new_id + 1);
                }
                new_records[new_id] = rec;

                ExtId ext = id_map_.getExt(old_id);
                if (!ext.empty()) new_id_map.getOrCreate(ext, new_id);
            }

            // ── EdgeRecords و Adjacency ──
            std::unordered_map<EdgeId, EdgeRecord> new_edges;
            for (const auto& [old_eid, original] : edge_records_) {
                auto er=original;
                auto eit = edge_remap.find(old_eid);
                if (eit == edge_remap.end()) continue;
                EdgeId new_eid = eit->second;

                auto src_it = node_remap.find(er.src);
                auto dst_it = node_remap.find(er.dst);
                if (src_it == node_remap.end() || dst_it == node_remap.end()) continue;

                er.edge_id = new_eid;
                er.src     = src_it->second;
                er.dst     = dst_it->second;
                new_edges[new_eid] = er;

                AdjEntry fwd{er.dst, new_eid, er.type_id};
                AdjEntry rev{er.src, new_eid, er.type_id};
                new_adj[er.src].addOut(fwd);
                new_adj[er.dst].addIn(rev);
                if (er.flags & FLAG_UNDIRECTED) {
                    new_adj[er.dst].addOut({er.src,new_eid,er.type_id});
                    new_adj[er.src].addIn({er.dst,new_eid,er.type_id});
                }
            }

            // جایگزینی ساختارها
            node_records_ = std::move(new_records);
            adj_          = std::move(new_adj);
            edge_records_ = std::move(new_edges);
            id_map_       = std::move(new_id_map);
            active_node_count_.store(node_records_.size());
            active_edge_count_.store(edge_records_.size());

            // پاک کردن free stacks (بعد از compaction تمام رکوردها active هستند)
            while (!node_free_stack_.empty()) node_free_stack_.pop();
            while (!edge_free_stack_.empty()) edge_free_stack_.pop();

            next_dense_id_.store(static_cast<DenseId>(node_remap.size()));
            next_edge_id_.store(static_cast<EdgeId>(edge_remap.size()));

            version_.fetch_add(1);
            return true;
        }

        void LiveGraph::clear() {
            std::unique_lock lock(mutex_);
            if (bulk_failed_) throw std::runtime_error("bulk ingestion failed; reopen graph");
            adj_.clear();
            node_records_.clear();
            edge_records_.clear();
            id_map_ = DenseIdMap{};
            while (!node_free_stack_.empty()) node_free_stack_.pop();
            while (!edge_free_stack_.empty()) edge_free_stack_.pop();
            active_node_count_.store(0);
            active_edge_count_.store(0);
            next_dense_id_.store(0);
            next_edge_id_.store(0);
            version_.fetch_add(1);
        }

    } // namespace graph
} // namespace nexora
