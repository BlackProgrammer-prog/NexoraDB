//
// Created by HOME on 6/19/2026.
//

#include "GraphIdStore.h"
/**
 * @file graph/GraphIdStore.cpp
 * @brief پیاده‌سازی کامل GraphIdStore
 */

#include <cstring>
#include <rocksdb/write_batch.h>

namespace nexora {
    namespace graph {

// ══════════════════════════════════════════════════════════════
// §1  Key builders
// ══════════════════════════════════════════════════════════════

        namespace {
// prefix مخفی — با "graph:" شروع می‌شود تا با collection‌ها تداخل نداشته باشد
            constexpr char kHiddenPrefix[] = "graph:idmap:";
        } // namespace

        std::string GraphIdStore::keyPrefix() const {
            return std::string(kHiddenPrefix) + graph_name_ + ":";
        }

        std::string GraphIdStore::keyExtToDense(const ExtId& ext_id) const {
            return keyPrefix() + "ext:" + ext_id;
        }

        std::string GraphIdStore::keyDenseToExt(DenseId dense_id) const {
            // zero-padded برای lexicographic sort صحیح
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%020llu",
                          static_cast<unsigned long long>(dense_id));
            return keyPrefix() + "dns:" + buf;
        }

        std::string GraphIdStore::keySeqNode() const {
            return keyPrefix() + "seq:node";
        }

        std::string GraphIdStore::keySeqEdge() const {
            return keyPrefix() + "seq:edge";
        }

        std::string GraphIdStore::keyNodeType(const std::string& name) const {
            return keyPrefix() + "ntype:" + name;
        }

        std::string GraphIdStore::keyEdgeType(const std::string& name) const {
            return keyPrefix() + "etype:" + name;
        }

        std::string GraphIdStore::keyMeta() const {
            return keyPrefix() + "meta";
        }

// ══════════════════════════════════════════════════════════════
// §2  Encode / Decode
// ══════════════════════════════════════════════════════════════

        std::string GraphIdStore::encodeDenseId(DenseId id) {
            // little-endian 8 bytes
            std::string s(8, '\0');
            uint64_t v = static_cast<uint64_t>(id);
            std::memcpy(&s[0], &v, 8);
            return s;
        }

        DenseId GraphIdStore::decodeDenseId(const std::string& s) {
            if (s.size() < 8) return kInvalidDenseId;
            uint64_t v = 0;
            std::memcpy(&v, s.data(), 8);
            return static_cast<DenseId>(v);
        }

        std::string GraphIdStore::encodeTypeId(TypeId id) {
            std::string s(4, '\0');
            uint32_t v = static_cast<uint32_t>(id);
            std::memcpy(&s[0], &v, 4);
            return s;
        }

        TypeId GraphIdStore::decodeTypeId(const std::string& s) {
            if (s.size() < 4) return kInvalidTypeId;
            uint32_t v = 0;
            std::memcpy(&v, s.data(), 4);
            return static_cast<TypeId>(v);
        }

// ══════════════════════════════════════════════════════════════
// §3  سازنده
// ══════════════════════════════════════════════════════════════

        GraphIdStore::GraphIdStore(rocksdb::DB* db, const std::string& graph_name)
                : db_(db), graph_name_(graph_name) {
            write_opts_.sync = true;  // DenseId map باید durable باشد
        }

// ══════════════════════════════════════════════════════════════
// §4  DenseId ↔ ExtId mapping
// ══════════════════════════════════════════════════════════════

        bool GraphIdStore::putMapping(const ExtId& ext_id, DenseId dense_id) {
            if (!db_ || ext_id.empty() || dense_id == kInvalidDenseId) return false;

            // اتمیک: هر دو جهت در یک WriteBatch
            rocksdb::WriteBatch batch;
            batch.Put(keyExtToDense(ext_id),   encodeDenseId(dense_id));
            batch.Put(keyDenseToExt(dense_id), ext_id);

            return db_->Write(write_opts_, &batch).ok();
        }

        bool GraphIdStore::deleteMapping(const ExtId& ext_id, DenseId dense_id) {
            if (!db_) return false;

            rocksdb::WriteBatch batch;
            batch.Delete(keyExtToDense(ext_id));
            batch.Delete(keyDenseToExt(dense_id));

            return db_->Write(write_opts_, &batch).ok();
        }

        std::optional<DenseId> GraphIdStore::getDenseId(const ExtId& ext_id) const {
            if (!db_) return std::nullopt;
            std::string val;
            rocksdb::Status s = db_->Get(read_opts_, keyExtToDense(ext_id), &val);
            if (!s.ok()) return std::nullopt;
            return decodeDenseId(val);
        }

        ExtId GraphIdStore::getExtId(DenseId dense_id) const {
            if (!db_ || dense_id == kInvalidDenseId) return "";
            std::string val;
            rocksdb::Status s = db_->Get(read_opts_, keyDenseToExt(dense_id), &val);
            return s.ok() ? val : "";
        }

        std::unordered_map<ExtId, DenseId> GraphIdStore::loadAllMappings() const {
            std::unordered_map<ExtId, DenseId> result;
            if (!db_) return result;

            // فقط مسیرهای ext: را اسکن می‌کنیم
            std::string ext_prefix = keyPrefix() + "ext:";
            rocksdb::ReadOptions ro;
            auto it = std::unique_ptr<rocksdb::Iterator>(db_->NewIterator(ro));

            for (it->Seek(ext_prefix);
                 it->Valid() && it->key().starts_with(ext_prefix);
                 it->Next()) {
                // ext_id = بخش بعد از prefix
                std::string key = it->key().ToString();
                ExtId ext_id = key.substr(ext_prefix.size());
                DenseId dense_id = decodeDenseId(it->value().ToString());
                if (dense_id != kInvalidDenseId) {
                    result[ext_id] = dense_id;
                }
            }

            return result;
        }

// ══════════════════════════════════════════════════════════════
// §5  Sequence Counters
// ══════════════════════════════════════════════════════════════

        bool GraphIdStore::saveNextDenseId(DenseId next_id) {
            if (!db_) return false;
            return db_->Put(write_opts_, keySeqNode(), encodeDenseId(next_id)).ok();
        }

        DenseId GraphIdStore::loadNextDenseId() const {
            if (!db_) return 0;
            std::string val;
            rocksdb::Status s = db_->Get(read_opts_, keySeqNode(), &val);
            return s.ok() ? decodeDenseId(val) : 0;
        }

        bool GraphIdStore::saveNextEdgeId(EdgeId next_id) {
            if (!db_) return false;
            return db_->Put(write_opts_, keySeqEdge(), encodeDenseId(next_id)).ok();
        }

        EdgeId GraphIdStore::loadNextEdgeId() const {
            if (!db_) return 0;
            std::string val;
            rocksdb::Status s = db_->Get(read_opts_, keySeqEdge(), &val);
            return s.ok() ? decodeDenseId(val) : 0;
        }

// ══════════════════════════════════════════════════════════════
// §6  Type Registry
// ══════════════════════════════════════════════════════════════

        bool GraphIdStore::putNodeType(const std::string& type_name, TypeId type_id) {
            if (!db_) return false;
            return db_->Put(write_opts_,
                            keyNodeType(type_name),
                            encodeTypeId(type_id)).ok();
        }

        bool GraphIdStore::putEdgeType(const std::string& type_name, TypeId type_id) {
            if (!db_) return false;
            return db_->Put(write_opts_,
                            keyEdgeType(type_name),
                            encodeTypeId(type_id)).ok();
        }

        std::unordered_map<std::string, TypeId> GraphIdStore::loadNodeTypes() const {
            std::unordered_map<std::string, TypeId> result;
            if (!db_) return result;

            std::string prefix = keyPrefix() + "ntype:";
            rocksdb::ReadOptions ro;
            auto it = std::unique_ptr<rocksdb::Iterator>(db_->NewIterator(ro));

            for (it->Seek(prefix);
                 it->Valid() && it->key().starts_with(prefix);
                 it->Next()) {
                std::string name = it->key().ToString().substr(prefix.size());
                TypeId tid = decodeTypeId(it->value().ToString());
                result[name] = tid;
            }
            return result;
        }

        std::unordered_map<std::string, TypeId> GraphIdStore::loadEdgeTypes() const {
            std::unordered_map<std::string, TypeId> result;
            if (!db_) return result;

            std::string prefix = keyPrefix() + "etype:";
            rocksdb::ReadOptions ro;
            auto it = std::unique_ptr<rocksdb::Iterator>(db_->NewIterator(ro));

            for (it->Seek(prefix);
                 it->Valid() && it->key().starts_with(prefix);
                 it->Next()) {
                std::string name = it->key().ToString().substr(prefix.size());
                TypeId tid = decodeTypeId(it->value().ToString());
                result[name] = tid;
            }
            return result;
        }

// ══════════════════════════════════════════════════════════════
// §7  GraphMeta backup در RocksDB
// ══════════════════════════════════════════════════════════════

        bool GraphIdStore::saveGraphMeta(const GraphMeta& meta) {
            if (!db_) return false;
            std::string data(reinterpret_cast<const char*>(&meta), sizeof(GraphMeta));
            return db_->Put(write_opts_, keyMeta(), data).ok();
        }

        std::optional<GraphMeta> GraphIdStore::loadGraphMeta() const {
            if (!db_) return std::nullopt;
            std::string data;
            rocksdb::Status s = db_->Get(read_opts_, keyMeta(), &data);
            if (!s.ok() || data.size() != sizeof(GraphMeta)) return std::nullopt;

            GraphMeta meta;
            std::memcpy(&meta, data.data(), sizeof(GraphMeta));
            if (meta.magic != 0x4E455847u) return std::nullopt;
            return meta;
        }

// ══════════════════════════════════════════════════════════════
// §8  Cleanup
// ══════════════════════════════════════════════════════════════

        bool GraphIdStore::clearAll() {
            if (!db_) return false;

            std::string prefix = keyPrefix();
            rocksdb::ReadOptions ro;
            auto it = std::unique_ptr<rocksdb::Iterator>(db_->NewIterator(ro));

            rocksdb::WriteBatch batch;
            for (it->Seek(prefix);
                 it->Valid() && it->key().starts_with(prefix);
                 it->Next()) {
                batch.Delete(it->key());
            }

            return db_->Write(write_opts_, &batch).ok();
        }

        size_t GraphIdStore::mappingCount() const {
            size_t count = 0;
            std::string ext_prefix = keyPrefix() + "ext:";
            rocksdb::ReadOptions ro;
            auto it = std::unique_ptr<rocksdb::Iterator>(db_->NewIterator(ro));
            for (it->Seek(ext_prefix);
                 it->Valid() && it->key().starts_with(ext_prefix);
                 it->Next()) {
                ++count;
            }
            return count;
        }

    } // namespace graph
} // namespace nexora