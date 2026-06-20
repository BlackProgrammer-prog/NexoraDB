//
// Created by HOME on 6/19/2026.
//

#include "Graphstorage.h"

/**
 * @file graph/GraphStorage.cpp
 * @brief پیاده‌سازی کامل GraphStorage — لایه دیسک
 */

#include <cassert>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace nexora {
    namespace graph {

// ══════════════════════════════════════════════════════════════
// §1  Helper: offset calculation
// ══════════════════════════════════════════════════════════════

        std::streamoff GraphStorage::nodeOffset(DenseId id) noexcept {
            return static_cast<std::streamoff>(id) *
                   static_cast<std::streamoff>(sizeof(NodeRecord));
        }

        std::streamoff GraphStorage::edgeOffset(EdgeId id) noexcept {
            return static_cast<std::streamoff>(id) *
                   static_cast<std::streamoff>(sizeof(EdgeRecord));
        }

// ══════════════════════════════════════════════════════════════
// §2  Lifecycle
// ══════════════════════════════════════════════════════════════

        GraphStorage::GraphStorage(const std::filesystem::path& dir,
                                   const std::string& name)
                : dir_(dir), name_(name) {
            namespace fs = std::filesystem;
            node_path_ = dir_ / (name_ + ".nex");
            edge_path_ = dir_ / (name_ + ".nexr");
            meta_path_ = dir_ / ("graph_meta.nexl");
        }

        GraphStorage::~GraphStorage() {
            close();
        }

        bool GraphStorage::openFile(std::fstream& f, const std::filesystem::path& path) {
            // اگر وجود دارد: open for read/write. اگر نه: create.
            if (std::filesystem::exists(path)) {
                f.open(path, std::ios::in | std::ios::out | std::ios::binary);
            } else {
                f.open(path, std::ios::in | std::ios::out | std::ios::binary |
                             std::ios::trunc);
            }
            return f.is_open() && f.good();
        }

        bool GraphStorage::open() {
            namespace fs = std::filesystem;

            // ایجاد پوشه در صورت نیاز
            std::error_code ec;
            fs::create_directories(dir_, ec);
            if (ec) return false;

            std::lock_guard<std::mutex> ln(node_mutex_);
            std::lock_guard<std::mutex> le(edge_mutex_);
            std::lock_guard<std::mutex> lm(meta_mutex_);

            if (!openFile(node_file_, node_path_)) return false;
            if (!openFile(edge_file_, edge_path_)) return false;
            if (!openFile(meta_file_, meta_path_)) return false;

            // محاسبه تعداد رکوردهای موجود
            node_file_.seekg(0, std::ios::end);
            uint64_t node_bytes = static_cast<uint64_t>(node_file_.tellg());
            node_file_records_  = node_bytes / sizeof(NodeRecord);

            edge_file_.seekg(0, std::ios::end);
            uint64_t edge_bytes = static_cast<uint64_t>(edge_file_.tellg());
            edge_file_records_  = edge_bytes / sizeof(EdgeRecord);

            is_open_ = true;
            return true;
        }

        void GraphStorage::close() {
            if (!is_open_) return;
            sync();
            std::lock_guard<std::mutex> ln(node_mutex_);
            std::lock_guard<std::mutex> le(edge_mutex_);
            std::lock_guard<std::mutex> lm(meta_mutex_);
            if (node_file_.is_open()) node_file_.close();
            if (edge_file_.is_open()) edge_file_.close();
            if (meta_file_.is_open()) meta_file_.close();
            is_open_ = false;
        }

        void GraphStorage::sync() {
            if (!is_open_) return;
            // flush تمام bufferها به OS
            {
                std::lock_guard<std::mutex> ln(node_mutex_);
                if (node_file_.is_open()) node_file_.flush();
            }
            {
                std::lock_guard<std::mutex> le(edge_mutex_);
                if (edge_file_.is_open()) edge_file_.flush();
            }
            {
                std::lock_guard<std::mutex> lm(meta_mutex_);
                if (meta_file_.is_open()) meta_file_.flush();
            }
        }

// ══════════════════════════════════════════════════════════════
// §3  Meta File (.nexl)
// ══════════════════════════════════════════════════════════════

        std::optional<GraphMeta> GraphStorage::readMeta() const {
            std::lock_guard<std::mutex> lm(meta_mutex_);
            meta_file_.seekg(0, std::ios::beg);
            if (!meta_file_.good()) return std::nullopt;

            GraphMeta meta;
            meta_file_.read(reinterpret_cast<char*>(&meta), sizeof(GraphMeta));
            if (meta_file_.gcount() != sizeof(GraphMeta)) return std::nullopt;
            if (meta.magic != 0x4E455847u) return std::nullopt;  // magic check
            return meta;
        }

        bool GraphStorage::writeMeta(const GraphMeta& meta) {
            std::lock_guard<std::mutex> lm(meta_mutex_);
            meta_file_.seekp(0, std::ios::beg);
            meta_file_.write(reinterpret_cast<const char*>(&meta), sizeof(GraphMeta));
            meta_file_.flush();
            return meta_file_.good();
        }

        bool GraphStorage::updateMetaState(GraphState state) {
            // offset state در struct = بعد از magic(4) + version(4) + kind(1) = 9 bytes
            constexpr std::streamoff state_offset =
                    sizeof(uint32_t) + sizeof(uint32_t) + sizeof(GraphKind);

            std::lock_guard<std::mutex> lm(meta_mutex_);
            meta_file_.seekp(state_offset, std::ios::beg);
            meta_file_.write(reinterpret_cast<const char*>(&state), sizeof(GraphState));
            meta_file_.flush();
            return meta_file_.good();
        }

        bool GraphStorage::isMetaClean() const {
            auto meta = readMeta();
            if (!meta) return false;
            return meta->state == GraphState::Clean;
        }

// ══════════════════════════════════════════════════════════════
// §4  Node File (.nex)
// ══════════════════════════════════════════════════════════════

        bool GraphStorage::writeNode(const NodeRecord& rec) {
            std::lock_guard<std::mutex> ln(node_mutex_);
            std::streamoff off = nodeOffset(rec.dense_id);
            node_file_.seekp(off, std::ios::beg);
            node_file_.write(reinterpret_cast<const char*>(&rec), sizeof(NodeRecord));
            node_file_.flush();
            if (!node_file_.good()) return false;

            // به‌روزرسانی cache تعداد رکوردها
            if (rec.dense_id >= node_file_records_)
                node_file_records_ = rec.dense_id + 1;

            return true;
        }

        std::optional<NodeRecord> GraphStorage::readNode(DenseId id) const {
            std::lock_guard<std::mutex> ln(node_mutex_);
            if (id >= node_file_records_) return std::nullopt;

            std::streamoff off = nodeOffset(id);
            node_file_.seekg(off, std::ios::beg);

            NodeRecord rec{};
            node_file_.read(reinterpret_cast<char*>(&rec), sizeof(NodeRecord));
            if (node_file_.gcount() != sizeof(NodeRecord)) return std::nullopt;
            return rec;
        }

        bool GraphStorage::updateNodeFlags(DenseId id, uint32_t flags) {
            std::lock_guard<std::mutex> ln(node_mutex_);
            if (id >= node_file_records_) return false;

            // offset flags در NodeRecord: بعد از dense_id(8) + type_id(4) = 12
            constexpr std::streamoff flags_off = sizeof(uint64_t) + sizeof(uint32_t);
            std::streamoff base = nodeOffset(id);

            node_file_.seekp(base + flags_off, std::ios::beg);
            node_file_.write(reinterpret_cast<const char*>(&flags), sizeof(uint32_t));
            node_file_.flush();
            return node_file_.good();
        }

        bool GraphStorage::updateNodeDegree(DenseId id, uint64_t out_degree,
                                            uint64_t in_degree) {
            std::lock_guard<std::mutex> ln(node_mutex_);
            if (id >= node_file_records_) return false;

            // offset out_degree در NodeRecord: بعد از dense_id(8)+type_id(4)+flags(4) = 16
            constexpr std::streamoff deg_off =
                    sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint32_t);
            std::streamoff base = nodeOffset(id);

            node_file_.seekp(base + deg_off, std::ios::beg);
            node_file_.write(reinterpret_cast<const char*>(&out_degree), sizeof(uint64_t));
            node_file_.write(reinterpret_cast<const char*>(&in_degree),  sizeof(uint64_t));
            node_file_.flush();
            return node_file_.good();
        }

        void GraphStorage::scanAllNodes(
                const std::function<bool(const NodeRecord&)>& callback) const
        {
            std::lock_guard<std::mutex> ln(node_mutex_);
            node_file_.seekg(0, std::ios::beg);
            NodeRecord rec{};
            while (node_file_.read(reinterpret_cast<char*>(&rec), sizeof(NodeRecord))) {
                if (!callback(rec)) break;
            }
            node_file_.clear();  // EOF flag clear
        }

// ══════════════════════════════════════════════════════════════
// §5  Edge File (.nexr)
// ══════════════════════════════════════════════════════════════

        bool GraphStorage::writeEdge(const EdgeRecord& rec) {
            std::lock_guard<std::mutex> le(edge_mutex_);
            std::streamoff off = edgeOffset(rec.edge_id);
            edge_file_.seekp(off, std::ios::beg);
            edge_file_.write(reinterpret_cast<const char*>(&rec), sizeof(EdgeRecord));
            edge_file_.flush();
            if (!edge_file_.good()) return false;

            if (rec.edge_id >= edge_file_records_)
                edge_file_records_ = rec.edge_id + 1;

            return true;
        }

        std::optional<EdgeRecord> GraphStorage::readEdge(EdgeId id) const {
            std::lock_guard<std::mutex> le(edge_mutex_);
            if (id >= edge_file_records_) return std::nullopt;

            std::streamoff off = edgeOffset(id);
            edge_file_.seekg(off, std::ios::beg);

            EdgeRecord rec{};
            edge_file_.read(reinterpret_cast<char*>(&rec), sizeof(EdgeRecord));
            if (edge_file_.gcount() != sizeof(EdgeRecord)) return std::nullopt;
            return rec;
        }

        bool GraphStorage::updateEdgeFlags(EdgeId id, uint32_t flags) {
            std::lock_guard<std::mutex> le(edge_mutex_);
            if (id >= edge_file_records_) return false;

            // offset flags در EdgeRecord: بعد از edge_id(8)+src(8)+dst(8)+type_id(4) = 28
            constexpr std::streamoff flags_off =
                    sizeof(uint64_t) * 3 + sizeof(uint32_t);
            std::streamoff base = edgeOffset(id);

            edge_file_.seekp(base + flags_off, std::ios::beg);
            edge_file_.write(reinterpret_cast<const char*>(&flags), sizeof(uint32_t));
            edge_file_.flush();
            return edge_file_.good();
        }

        void GraphStorage::scanAllEdges(
                const std::function<bool(const EdgeRecord&)>& callback) const
        {
            std::lock_guard<std::mutex> le(edge_mutex_);
            edge_file_.seekg(0, std::ios::beg);
            EdgeRecord rec{};
            while (edge_file_.read(reinterpret_cast<char*>(&rec), sizeof(EdgeRecord))) {
                if (!callback(rec)) break;
            }
            edge_file_.clear();
        }

// ══════════════════════════════════════════════════════════════
// §6  Compaction
// ══════════════════════════════════════════════════════════════

        bool GraphStorage::compactNodes(std::unordered_map<DenseId, DenseId>& id_remap) {
            // ۱. اسکن فایل، جمع‌آوری رکوردهای ACTIVE
            std::vector<NodeRecord> active_records;
            scanAllNodes([&](const NodeRecord& rec) -> bool {
                if (rec.isActive()) active_records.push_back(rec);
                return true;
            });

            // ۲. ساخت remap
            id_remap.clear();
            for (size_t i = 0; i < active_records.size(); ++i) {
                DenseId old_id = active_records[i].dense_id;
                DenseId new_id = static_cast<DenseId>(i);
                id_remap[old_id] = new_id;
                active_records[i].dense_id = new_id;
            }

            // ۳. بازنویسی فایل (truncate + rewrite)
            {
                std::lock_guard<std::mutex> ln(node_mutex_);
                node_file_.close();
                node_file_.open(node_path_,
                                std::ios::in | std::ios::out | std::ios::binary |
                                std::ios::trunc);
                if (!node_file_.is_open()) return false;

                for (const auto& rec : active_records) {
                    node_file_.write(reinterpret_cast<const char*>(&rec),
                                     sizeof(NodeRecord));
                }
                node_file_.flush();
                node_file_records_ = active_records.size();
            }

            return node_file_.good();
        }

        bool GraphStorage::compactEdges(std::unordered_map<EdgeId, EdgeId>& id_remap) {
            std::vector<EdgeRecord> active_records;
            scanAllEdges([&](const EdgeRecord& rec) -> bool {
                if (rec.isActive()) active_records.push_back(rec);
                return true;
            });

            id_remap.clear();
            for (size_t i = 0; i < active_records.size(); ++i) {
                EdgeId old_id = active_records[i].edge_id;
                EdgeId new_id = static_cast<EdgeId>(i);
                id_remap[old_id] = new_id;
                active_records[i].edge_id = new_id;
            }

            {
                std::lock_guard<std::mutex> le(edge_mutex_);
                edge_file_.close();
                edge_file_.open(edge_path_,
                                std::ios::in | std::ios::out | std::ios::binary |
                                std::ios::trunc);
                if (!edge_file_.is_open()) return false;

                for (const auto& rec : active_records) {
                    edge_file_.write(reinterpret_cast<const char*>(&rec),
                                     sizeof(EdgeRecord));
                }
                edge_file_.flush();
                edge_file_records_ = active_records.size();
            }

            return edge_file_.good();
        }

    } // namespace graph
} // namespace nexora