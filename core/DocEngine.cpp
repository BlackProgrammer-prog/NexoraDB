//
// Created by HOME on 6/4/2026.
//

/**
 * @file DocEngine.cpp
 * @brief پیاده‌سازی کامل موتور اسناد NexoraDB
 *
 * @details
 * وابستگی‌ها:
 *   - RocksDB (از طریق vcpkg)
 *   - nexora::query (Condition, UpdateSpec, Evaluator) — از query/ می‌آید
 *
 * @note
 * در این نسخه placeholder لایه query حذف شده و به‌جای آن
 * از فایل‌های واقعی query/ استفاده می‌شود.
 * منطق Match و Apply کاملاً به Evaluator واگذار شده است.
 */

#include "DocEngine.h"

// RocksDB
#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/status.h>
#include <rocksdb/utilities/transaction.h>
#include <rocksdb/utilities/transaction_db.h>
#include <rocksdb/write_batch.h>

// Standard Library
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────
// ماکروهای کمکی
// ─────────────────────────────────────────────────────────────

#ifdef NEXORA_DEBUG
#define NX_LOG(msg) std::cerr << "[NexoraDB] " << msg << "\n"
#else
#define NX_LOG(msg) ((void)0)
#endif

/// بررسی وضعیت RocksDB و بازگشت DBResult در صورت خطا
#define ROCKS_CHECK(status, op_name)                                   \
  if (!(status).ok()) {                                                \
    return DBResult::Err("[RocksDB] " op_name ": " +                  \
                         (status).ToString());                         \
  }

namespace nexora {
    namespace core {

// ══════════════════════════════════════════════════════════════
// §1  ثابت‌های prefix کلید RocksDB
// ══════════════════════════════════════════════════════════════

        namespace keys {
            constexpr char kData[]    = "data:";      ///< data:{col}:{id}
            constexpr char kMetaCol[] = "meta:col:";  ///< meta:col:{name}
            constexpr char kMetaIdx[] = "meta:idx:";  ///< meta:idx:{col}:{name}
            constexpr char kMetaFk[]  = "meta:fk:";   ///< meta:fk:{col}:{name}
            constexpr char kIdx[]     = "idx:";       ///< idx:{col}:{field}:{val}:{id}
            constexpr char kSeq[]     = "seq:";       ///< seq:{col} — شمارنده اسناد
        } // namespace keys

// ══════════════════════════════════════════════════════════════
// §2  Serialization ساده برای metadata
//     (در production از protobuf یا flatbuffers استفاده کنید)
// ══════════════════════════════════════════════════════════════

        namespace serial {

/// فرمت: [4 بایت length LE][bytes]
            static void WriteString(std::string& out, const std::string& s) {
                uint32_t len = static_cast<uint32_t>(s.size());
                out.append(reinterpret_cast<const char*>(&len), 4);
                out.append(s);
            }

            static bool ReadString(const std::string& in, size_t& pos, std::string& out) {
                if (pos + 4 > in.size()) return false;
                uint32_t len;
                std::memcpy(&len, in.data() + pos, 4);
                pos += 4;
                if (pos + len > in.size()) return false;
                out = in.substr(pos, len);
                pos += len;
                return true;
            }

            static void WriteByte(std::string& out, uint8_t b) {
                out.push_back(static_cast<char>(b));
            }

            static bool ReadByte(const std::string& in, size_t& pos, uint8_t& b) {
                if (pos >= in.size()) return false;
                b = static_cast<uint8_t>(in[pos++]);
                return true;
            }

            static void WriteBool(std::string& out, bool v) {
                WriteByte(out, v ? 1 : 0);
            }

            static bool ReadBool(const std::string& in, size_t& pos, bool& v) {
                uint8_t b;
                if (!ReadByte(in, pos, b)) return false;
                v = (b != 0);
                return true;
            }

        } // namespace serial

// ══════════════════════════════════════════════════════════════
// §3  متدهای Static خصوصی
// ══════════════════════════════════════════════════════════════

        std::string DocEngine::MakeDocKey(const std::string& collection,
                                          const std::string& doc_id) {
            return std::string(keys::kData) + collection + ":" + doc_id;
        }

        std::string DocEngine::MakeMetaKey(const std::string& type,
                                           const std::string& collection,
                                           const std::string& name) {
            std::string key = type + collection;
            if (!name.empty()) key += ":" + name;
            return key;
        }

        std::string DocEngine::MakeIndexKey(const std::string& collection,
                                            const std::string& field,
                                            const std::string& value,
                                            const std::string& doc_id) {
            return std::string(keys::kIdx) + collection + ":" + field + ":" + value + ":" + doc_id;
        }

        std::string DocEngine::GenerateDocId() {
            static thread_local std::mt19937 rng(
                    static_cast<uint32_t>(
                            std::chrono::steady_clock::now().time_since_epoch().count()));
            std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFFu);
            std::ostringstream ss;
            ss << std::hex << std::setfill('0')
               << std::setw(8) << dist(rng) << "-"
               << std::setw(4) << (dist(rng) & 0xFFFFu) << "-"
               << std::setw(4) << ((dist(rng) & 0x0FFFu) | 0x4000u) << "-"
               << std::setw(4) << ((dist(rng) & 0x3FFFu) | 0x8000u) << "-"
               << std::setw(8) << dist(rng)
               << std::setw(4) << (dist(rng) & 0xFFFFu);
            return ss.str();
        }

// ──────────────────────────────────────────────────────────────
// §3.1  تفویض به QueryLayer
//        این سه متد خصوصی تنها نقطه ارتباط DocEngine با Evaluator هستند.
//        thread_local برای جلوگیری از ساخت مکرر Evaluator در هر فراخوانی.
// ──────────────────────────────────────────────────────────────

        std::string DocEngine::ExtractField(const std::string& bson,
                                            const std::string& field_name) {
            thread_local nexora::query::Evaluator eval;
            auto fv = eval.ExtractField(bson, field_name);
            return fv.found ? fv.raw : "";
        }

        bool DocEngine::MatchesCondition(const std::string&              bson,
                                         const nexora::query::Condition& condition) {
            thread_local nexora::query::Evaluator eval;
            return eval.Match(bson, condition);
        }

        std::string DocEngine::ApplyUpdate(const std::string&               bson,
                                           const nexora::query::UpdateSpec& spec) {
            thread_local nexora::query::Evaluator eval;
            return eval.Apply(bson, spec);
        }

// ──────────────────────────────────────────────────────────────
// §3.2  Serialization برای Schema
// ──────────────────────────────────────────────────────────────

        std::string DocEngine::SerializeSchema(const SchemaDefinition& schema) {
            std::string out;
            serial::WriteBool(out, schema.strict);
            uint32_t fc = static_cast<uint32_t>(schema.fields.size());
            out.append(reinterpret_cast<const char*>(&fc), 4);
            for (const auto& f : schema.fields) {
                serial::WriteString(out, f.name);
                serial::WriteByte(out, static_cast<uint8_t>(f.type));
                serial::WriteBool(out, f.required);
                serial::WriteBool(out, f.unique);
                bool has_def = f.default_val.has_value();
                serial::WriteBool(out, has_def);
                if (has_def) serial::WriteString(out, *f.default_val);
            }
            return out;
        }

        std::optional<SchemaDefinition> DocEngine::DeserializeSchema(const std::string& bytes) {
            if (bytes.empty()) return std::nullopt;
            size_t pos = 0;
            SchemaDefinition schema;
            if (!serial::ReadBool(bytes, pos, schema.strict)) return std::nullopt;
            uint32_t fc = 0;
            if (pos + 4 > bytes.size()) return std::nullopt;
            std::memcpy(&fc, bytes.data() + pos, 4);
            pos += 4;
            for (uint32_t i = 0; i < fc; ++i) {
                SchemaField f;
                uint8_t tb;
                bool hd;
                if (!serial::ReadString(bytes, pos, f.name))     return std::nullopt;
                if (!serial::ReadByte(bytes, pos, tb))            return std::nullopt;
                f.type = static_cast<FieldType>(tb);
                if (!serial::ReadBool(bytes, pos, f.required))   return std::nullopt;
                if (!serial::ReadBool(bytes, pos, f.unique))     return std::nullopt;
                if (!serial::ReadBool(bytes, pos, hd))            return std::nullopt;
                if (hd) {
                    std::string dv;
                    if (!serial::ReadString(bytes, pos, dv)) return std::nullopt;
                    f.default_val = dv;
                }
                schema.fields.push_back(std::move(f));
            }
            return schema;
        }

        std::string DocEngine::SerializeIndex(const IndexDefinition& idx) {
            std::string out;
            serial::WriteString(out, idx.index_name);
            serial::WriteByte(out, static_cast<uint8_t>(idx.type));
            uint32_t fc = static_cast<uint32_t>(idx.fields.size());
            out.append(reinterpret_cast<const char*>(&fc), 4);
            for (const auto& f : idx.fields) serial::WriteString(out, f);
            return out;
        }

        std::optional<IndexDefinition> DocEngine::DeserializeIndex(const std::string& bytes) {
            if (bytes.empty()) return std::nullopt;
            size_t pos = 0;
            IndexDefinition idx;
            uint8_t tb;
            if (!serial::ReadString(bytes, pos, idx.index_name)) return std::nullopt;
            if (!serial::ReadByte(bytes, pos, tb))                return std::nullopt;
            idx.type = static_cast<IndexType>(tb);
            uint32_t fc = 0;
            if (pos + 4 > bytes.size()) return std::nullopt;
            std::memcpy(&fc, bytes.data() + pos, 4);
            pos += 4;
            for (uint32_t i = 0; i < fc; ++i) {
                std::string f;
                if (!serial::ReadString(bytes, pos, f)) return std::nullopt;
                idx.fields.push_back(std::move(f));
            }
            return idx;
        }

        std::string DocEngine::SerializeFk(const ForeignKeyDefinition& fk) {
            std::string out;
            serial::WriteString(out, fk.fk_name);
            serial::WriteString(out, fk.local_field);
            serial::WriteString(out, fk.ref_collection);
            serial::WriteString(out, fk.ref_field);
            return out;
        }

        std::optional<ForeignKeyDefinition> DocEngine::DeserializeFk(const std::string& bytes) {
            if (bytes.empty()) return std::nullopt;
            size_t pos = 0;
            ForeignKeyDefinition fk;
            if (!serial::ReadString(bytes, pos, fk.fk_name))        return std::nullopt;
            if (!serial::ReadString(bytes, pos, fk.local_field))    return std::nullopt;
            if (!serial::ReadString(bytes, pos, fk.ref_collection)) return std::nullopt;
            if (!serial::ReadString(bytes, pos, fk.ref_field))      return std::nullopt;
            return fk;
        }

// ══════════════════════════════════════════════════════════════
// §4  سازنده و مخرب
// ══════════════════════════════════════════════════════════════

        DocEngine::DocEngine(const std::string& db_path)
                : db_path_(db_path) {

            db_options_.create_if_missing              = true;
            db_options_.create_missing_column_families = true;
            db_options_.compression                    = rocksdb::kSnappyCompression;
            db_options_.max_open_files                 = 500;
            db_options_.write_buffer_size             = 64 * 1024 * 1024;  // 64 MB
            db_options_.max_write_buffer_number       = 3;
            db_options_.target_file_size_base         = 64 * 1024 * 1024;

            write_options_.sync         = false;  // برای performance
            read_options_.verify_checksums = true;

            rocksdb::TransactionDB* raw_db = nullptr;
            rocksdb::Status s = rocksdb::TransactionDB::Open(
                    db_options_, txn_db_options_, db_path_, &raw_db);

            if (!s.ok()) {
                throw std::runtime_error("[NexoraDB] Failed to open database at '" +
                                         db_path_ + "': " + s.ToString());
            }

            txn_db_.reset(raw_db, [](rocksdb::TransactionDB* db) {
                if (db) { db->SyncWAL(); delete db; }
            });

            NX_LOG("DocEngine opened at: " << db_path_);
        }

        DocEngine::~DocEngine() {
            NX_LOG("DocEngine closing at: " << db_path_);
        }

        bool DocEngine::IsHealthy() const noexcept {
            if (!txn_db_) return false;
            std::string prop;
            return txn_db_->GetProperty("rocksdb.num-live-versions", &prop);
        }

// ══════════════════════════════════════════════════════════════
// §5  مدیریت Collection و Schema
// ══════════════════════════════════════════════════════════════

        DBResult DocEngine::CreateCollection(
                const std::string& collection_name,
                const std::optional<SchemaDefinition>& schema) {

            if (collection_name.empty())
                return DBResult::Err("Collection name cannot be empty");

            std::string meta_key = MakeMetaKey(keys::kMetaCol, collection_name);
            std::string existing;
            rocksdb::Status s = txn_db_->Get(read_options_, meta_key, &existing);

            if (s.ok())
                return DBResult::Err("Collection '" + collection_name + "' already exists");
            if (!s.IsNotFound())
                return DBResult::Err("RocksDB error: " + s.ToString());

            SchemaDefinition eff_schema = schema.value_or(SchemaDefinition{});
            s = txn_db_->Put(write_options_, meta_key, SerializeSchema(eff_schema));
            ROCKS_CHECK(s, "CreateCollection metadata");

            s = txn_db_->Put(write_options_,
                             std::string(keys::kSeq) + collection_name, "0");
            ROCKS_CHECK(s, "CreateCollection seq");

            NX_LOG("Collection created: " << collection_name);
            return DBResult::Ok("Collection '" + collection_name + "' created");
        }

        DBResult DocEngine::DropCollection(const std::string& collection_name) {
            if (!CollectionExists(collection_name))
                return DBResult::Err("Collection '" + collection_name + "' does not exist");

            rocksdb::ReadOptions ro;
            auto it = std::unique_ptr<rocksdb::Iterator>(txn_db_->NewIterator(ro));
            rocksdb::WriteBatch batch;

            // حذف اسناد
            std::string data_prefix = std::string(keys::kData) + collection_name + ":";
            for (it->Seek(data_prefix);
                 it->Valid() && it->key().starts_with(data_prefix); it->Next())
                batch.Delete(it->key());

            // حذف index entries
            std::string idx_prefix = std::string(keys::kIdx) + collection_name + ":";
            for (it->Seek(idx_prefix);
                 it->Valid() && it->key().starts_with(idx_prefix); it->Next())
                batch.Delete(it->key());

            // حذف metadata index
            std::string meta_idx_prefix = std::string(keys::kMetaIdx) + collection_name + ":";
            for (it->Seek(meta_idx_prefix);
                 it->Valid() && it->key().starts_with(meta_idx_prefix); it->Next())
                batch.Delete(it->key());

            // حذف metadata FK
            std::string meta_fk_prefix = std::string(keys::kMetaFk) + collection_name + ":";
            for (it->Seek(meta_fk_prefix);
                 it->Valid() && it->key().starts_with(meta_fk_prefix); it->Next())
                batch.Delete(it->key());

            batch.Delete(MakeMetaKey(keys::kMetaCol, collection_name));
            batch.Delete(std::string(keys::kSeq) + collection_name);

            rocksdb::Status s = txn_db_->Write(write_options_, &batch);
            ROCKS_CHECK(s, "DropCollection");

            NX_LOG("Collection dropped: " << collection_name);
            return DBResult::Ok("Collection '" + collection_name + "' dropped");
        }

        DBResult DocEngine::SetSchemaValidation(const std::string&      collection_name,
                                                const SchemaDefinition& schema) {
            if (!CollectionExists(collection_name))
                return DBResult::Err("Collection '" + collection_name + "' does not exist");
            rocksdb::Status s = txn_db_->Put(write_options_,
                                             MakeMetaKey(keys::kMetaCol, collection_name),
                                             SerializeSchema(schema));
            ROCKS_CHECK(s, "SetSchemaValidation");
            return DBResult::Ok("Schema updated for '" + collection_name + "'");
        }

        bool DocEngine::CollectionExists(const std::string& collection_name) const {
            std::string val;
            return txn_db_->Get(read_options_,
                                MakeMetaKey(keys::kMetaCol, collection_name), &val).ok();
        }

// ══════════════════════════════════════════════════════════════
// §6  مدیریت Index
// ══════════════════════════════════════════════════════════════

        DBResult DocEngine::CreateIndex(const std::string&     collection_name,
                                        const IndexDefinition& index_def) {
            if (!CollectionExists(collection_name))
                return DBResult::Err("Collection '" + collection_name + "' does not exist");
            if (index_def.index_name.empty() || index_def.fields.empty())
                return DBResult::Err("Index must have a name and at least one field");

            std::string meta_key = MakeMetaKey(keys::kMetaIdx, collection_name,
                                               index_def.index_name);
            std::string ex;
            rocksdb::Status s = txn_db_->Get(read_options_, meta_key, &ex);
            if (s.ok())
                return DBResult::Err("Index '" + index_def.index_name + "' already exists");

            s = txn_db_->Put(write_options_, meta_key, SerializeIndex(index_def));
            ROCKS_CHECK(s, "CreateIndex metadata");

            // Rebuild index از داده‌های موجود
            std::string data_prefix = std::string(keys::kData) + collection_name + ":";
            rocksdb::ReadOptions ro;
            auto it = std::unique_ptr<rocksdb::Iterator>(txn_db_->NewIterator(ro));
            rocksdb::WriteBatch batch;
            uint64_t cnt = 0;

            for (it->Seek(data_prefix);
                 it->Valid() && it->key().starts_with(data_prefix); it->Next()) {
                std::string doc_id = it->key().ToString().substr(data_prefix.size());
                std::string bson   = it->value().ToString();
                for (const auto& field : index_def.fields) {
                    std::string val = ExtractField(bson, field);
                    if (!val.empty()) {
                        batch.Put(MakeIndexKey(collection_name, field, val, doc_id), doc_id);
                        ++cnt;
                    }
                }
            }

            s = txn_db_->Write(write_options_, &batch);
            ROCKS_CHECK(s, "CreateIndex rebuild");

            return DBResult::Ok("Index '" + index_def.index_name +
                                "' created (" + std::to_string(cnt) + " entries)");
        }

        DBResult DocEngine::DropIndex(const std::string& collection_name,
                                      const std::string& index_name) {
            std::string meta_key = MakeMetaKey(keys::kMetaIdx, collection_name, index_name);
            std::string ser;
            rocksdb::Status s = txn_db_->Get(read_options_, meta_key, &ser);
            if (!s.ok()) return DBResult::Err("Index '" + index_name + "' not found");

            auto idx_def = DeserializeIndex(ser);
            if (!idx_def) return DBResult::Err("Corrupted index metadata");

            rocksdb::WriteBatch batch;
            rocksdb::ReadOptions ro;
            auto it = std::unique_ptr<rocksdb::Iterator>(txn_db_->NewIterator(ro));
            for (const auto& field : idx_def->fields) {
                std::string ip = std::string(keys::kIdx) + collection_name + ":" + field + ":";
                for (it->Seek(ip); it->Valid() && it->key().starts_with(ip); it->Next())
                    batch.Delete(it->key());
            }
            batch.Delete(meta_key);

            s = txn_db_->Write(write_options_, &batch);
            ROCKS_CHECK(s, "DropIndex");
            return DBResult::Ok("Index '" + index_name + "' dropped");
        }

// ══════════════════════════════════════════════════════════════
// §7  مدیریت Foreign Key
// ══════════════════════════════════════════════════════════════

        DBResult DocEngine::AddForeignKey(const std::string&          collection_name,
                                          const ForeignKeyDefinition& fk_def) {
            if (!CollectionExists(collection_name))
                return DBResult::Err("Collection '" + collection_name + "' does not exist");
            if (!CollectionExists(fk_def.ref_collection))
                return DBResult::Err("Referenced collection '" +
                                     fk_def.ref_collection + "' does not exist");

            std::string meta_key = MakeMetaKey(keys::kMetaFk, collection_name, fk_def.fk_name);
            std::string ex;
            rocksdb::Status s = txn_db_->Get(read_options_, meta_key, &ex);
            if (s.ok())
                return DBResult::Err("Foreign key '" + fk_def.fk_name + "' already exists");

            s = txn_db_->Put(write_options_, meta_key, SerializeFk(fk_def));
            ROCKS_CHECK(s, "AddForeignKey");
            return DBResult::Ok("Foreign key '" + fk_def.fk_name + "' added");
        }

        DBResult DocEngine::DropForeignKey(const std::string& collection_name,
                                           const std::string& fk_name) {
            std::string meta_key = MakeMetaKey(keys::kMetaFk, collection_name, fk_name);
            std::string ex;
            rocksdb::Status s = txn_db_->Get(read_options_, meta_key, &ex);
            if (!s.ok())
                return DBResult::Err("Foreign key '" + fk_name + "' not found");
            s = txn_db_->Delete(write_options_, meta_key);
            ROCKS_CHECK(s, "DropForeignKey");
            return DBResult::Ok("Foreign key '" + fk_name + "' dropped");
        }

// ══════════════════════════════════════════════════════════════
// §8  متدهای کمکی Validation
// ══════════════════════════════════════════════════════════════

        bool DocEngine::ValidateDocument(const std::string&      bson_document,
                                         const SchemaDefinition& schema,
                                         std::string&            error_out) const {
            for (const auto& field : schema.fields) {
                if (field.required && ExtractField(bson_document, field.name).empty()) {
                    error_out = "Required field '" + field.name + "' is missing";
                    return false;
                }
            }
            return true;
        }

        bool DocEngine::CheckForeignKey(const ForeignKeyDefinition& fk,
                                        const std::string&          bson_document,
                                        std::string&                error_out) {
            std::string local_val = ExtractField(bson_document, fk.local_field);
            if (local_val.empty()) return true;  // فیلد وجود ندارد — بررسی لازم نیست

            // بررسی با direct key اگر ref_field == "_id"
            if (fk.ref_field == "_id") {
                std::string ref_key = MakeDocKey(fk.ref_collection, local_val);
                std::string ref_val;
                rocksdb::Status s = txn_db_->Get(read_options_, ref_key, &ref_val);
                if (s.IsNotFound()) {
                    error_out = "FK violation: '" + local_val +
                                "' not found in '" + fk.ref_collection + "'";
                    return false;
                }
                return true;
            }

            // جستجو از طریق ایندکس
            std::string idx_prefix = std::string(keys::kIdx) + fk.ref_collection +
                                     ":" + fk.ref_field + ":" + local_val + ":";
            rocksdb::ReadOptions ro;
            auto it = std::unique_ptr<rocksdb::Iterator>(txn_db_->NewIterator(ro));
            it->Seek(idx_prefix);
            if (!it->Valid() || !it->key().starts_with(idx_prefix)) {
                error_out = "FK violation: '" + local_val +
                            "' not found in '" + fk.ref_collection + "." + fk.ref_field + "'";
                return false;
            }
            return true;
        }

        rocksdb::Status DocEngine::UpdateIndexesOnInsert(const std::string& collection,
                                                         const std::string& doc_id,
                                                         const std::string& bson) {
            auto indexes = GetIndexes(collection);
            if (indexes.empty()) return rocksdb::Status::OK();
            rocksdb::WriteBatch batch;
            for (const auto& idx : indexes)
                for (const auto& field : idx.fields) {
                    std::string val = ExtractField(bson, field);
                    if (!val.empty())
                        batch.Put(MakeIndexKey(collection, field, val, doc_id), doc_id);
                }
            return txn_db_->Write(write_options_, &batch);
        }

        rocksdb::Status DocEngine::CleanIndexesOnDelete(const std::string& collection,
                                                        const std::string& doc_id,
                                                        const std::string& old_bson) {
            auto indexes = GetIndexes(collection);
            if (indexes.empty()) return rocksdb::Status::OK();
            rocksdb::WriteBatch batch;
            for (const auto& idx : indexes)
                for (const auto& field : idx.fields) {
                    std::string val = ExtractField(old_bson, field);
                    if (!val.empty())
                        batch.Delete(MakeIndexKey(collection, field, val, doc_id));
                }
            return txn_db_->Write(write_options_, &batch);
        }

// ══════════════════════════════════════════════════════════════
// §9  CRUD - Insert
// ══════════════════════════════════════════════════════════════

        DBResult DocEngine::InsertOne(const std::string& collection_name,
                                      const std::string& bson_document) {
            if (!CollectionExists(collection_name))
                return DBResult::Err("Collection '" + collection_name + "' does not exist");

            // Schema Validation
            auto schema = GetSchema(collection_name);
            if (schema && !schema->fields.empty()) {
                std::string err;
                if (!ValidateDocument(bson_document, *schema, err))
                    return DBResult::Err("Schema validation failed: " + err);
            }

            // Foreign Key Check
            for (const auto& fk : GetForeignKeys(collection_name)) {
                std::string err;
                if (!CheckForeignKey(fk, bson_document, err))
                    return DBResult::Err(err);
            }

            // تولید یا استخراج doc_id
            std::string doc_id = ExtractField(bson_document, "_id");
            if (doc_id.empty()) doc_id = GenerateDocId();

            rocksdb::Status s = txn_db_->Put(write_options_,
                                             MakeDocKey(collection_name, doc_id),
                                             bson_document);
            ROCKS_CHECK(s, "InsertOne");

            // به‌روزرسانی ایندکس‌ها
            auto idx_status = UpdateIndexesOnInsert(collection_name, doc_id, bson_document);
            if (!idx_status.ok())
                NX_LOG("Warning: Index update failed after InsertOne: " << idx_status.ToString());

            // به‌روزرسانی شمارنده
            std::string seq_key = std::string(keys::kSeq) + collection_name;
            std::string seq_val;
            txn_db_->Get(read_options_, seq_key, &seq_val);
            int64_t cnt = seq_val.empty() ? 1 : std::stoll(seq_val) + 1;
            txn_db_->Put(write_options_, seq_key, std::to_string(cnt));

            return DBResult::Ok(doc_id);
        }

        DBResult DocEngine::InsertMany(const std::string&              collection_name,
                                       const std::vector<std::string>& bson_documents) {
            if (!CollectionExists(collection_name))
                return DBResult::Err("Collection '" + collection_name + "' does not exist");
            if (bson_documents.empty())
                return DBResult::Err("Document list is empty");

            auto schema  = GetSchema(collection_name);
            auto fks     = GetForeignKeys(collection_name);
            auto indexes = GetIndexes(collection_name);

            rocksdb::WriteBatch batch;
            std::vector<std::string> created_ids;
            created_ids.reserve(bson_documents.size());

            for (const auto& bson : bson_documents) {
                if (schema && !schema->fields.empty()) {
                    std::string err;
                    if (!ValidateDocument(bson, *schema, err))
                        return DBResult::Err("Schema validation failed: " + err);
                }
                for (const auto& fk : fks) {
                    std::string err;
                    if (!CheckForeignKey(fk, bson, err))
                        return DBResult::Err(err);
                }

                std::string doc_id = ExtractField(bson, "_id");
                if (doc_id.empty()) doc_id = GenerateDocId();

                batch.Put(MakeDocKey(collection_name, doc_id), bson);
                created_ids.push_back(doc_id);

                for (const auto& idx : indexes)
                    for (const auto& field : idx.fields) {
                        std::string val = ExtractField(bson, field);
                        if (!val.empty())
                            batch.Put(MakeIndexKey(collection_name, field, val, doc_id), doc_id);
                    }
            }

            rocksdb::Status s = txn_db_->Write(write_options_, &batch);
            ROCKS_CHECK(s, "InsertMany");

            // به‌روزرسانی شمارنده
            std::string seq_key = std::string(keys::kSeq) + collection_name;
            std::string seq_val;
            txn_db_->Get(read_options_, seq_key, &seq_val);
            int64_t cnt = (seq_val.empty() ? 0 : std::stoll(seq_val)) +
                          static_cast<int64_t>(created_ids.size());
            txn_db_->Put(write_options_, seq_key, std::to_string(cnt));

            // ساخت JSON آرایه IDs
            std::string ids_json = "[";
            for (size_t i = 0; i < created_ids.size(); ++i) {
                if (i > 0) ids_json += ",";
                ids_json += "\"" + created_ids[i] + "\"";
            }
            ids_json += "]";
            return DBResult::Ok(ids_json);
        }

// ══════════════════════════════════════════════════════════════
// §10  CRUD - Find
// ══════════════════════════════════════════════════════════════

        DBResult DocEngine::FindById(const std::string& collection_name,
                                     const std::string& doc_id) {
            std::string bson;
            rocksdb::Status s = txn_db_->Get(read_options_,
                                             MakeDocKey(collection_name, doc_id),
                                             &bson);
            if (s.IsNotFound())
                return DBResult::Err("Document '" + doc_id + "' not found in '" +
                                     collection_name + "'");
            ROCKS_CHECK(s, "FindById");
            return DBResult::Ok(bson);
        }

        DBResult DocEngine::FindMany(const std::string&              collection_name,
                                     const nexora::query::Condition& condition,
                                     uint32_t                        limit,
                                     uint32_t                        skip) {
            if (!CollectionExists(collection_name))
                return DBResult::Err("Collection '" + collection_name + "' does not exist");

            std::string data_prefix = std::string(keys::kData) + collection_name + ":";
            rocksdb::ReadOptions ro;
            auto it = std::unique_ptr<rocksdb::Iterator>(txn_db_->NewIterator(ro));

            std::string results_json = "[";
            uint32_t count   = 0;
            uint32_t skipped = 0;
            bool first = true;

            for (it->Seek(data_prefix);
                 it->Valid() && it->key().starts_with(data_prefix); it->Next()) {

                std::string bson = it->value().ToString();
                if (!MatchesCondition(bson, condition)) continue;
                if (skipped < skip) { ++skipped; continue; }

                if (!first) results_json += ",";
                results_json += bson;
                first = false;
                ++count;

                if (limit > 0 && count >= limit) break;
            }

            results_json += "]";
            return DBResult::Ok(results_json);
        }

// ══════════════════════════════════════════════════════════════
// §11  CRUD - Update
// ══════════════════════════════════════════════════════════════

        DBResult DocEngine::UpdateById(const std::string&               collection_name,
                                       const std::string&               doc_id,
                                       const nexora::query::UpdateSpec& update_spec) {
            auto read_result = FindById(collection_name, doc_id);
            if (!read_result.success) return read_result;

            std::string old_bson = read_result.data;
            std::string new_bson = ApplyUpdate(old_bson, update_spec);

            // FK check روی فیلدهای تغییر یافته
            for (const auto& fk : GetForeignKeys(collection_name)) {
                std::string err;
                if (!CheckForeignKey(fk, new_bson, err))
                    return DBResult::Err(err);
            }

            CleanIndexesOnDelete(collection_name, doc_id, old_bson);

            rocksdb::Status s = txn_db_->Put(write_options_,
                                             MakeDocKey(collection_name, doc_id),
                                             new_bson);
            ROCKS_CHECK(s, "UpdateById");

            UpdateIndexesOnInsert(collection_name, doc_id, new_bson);
            return DBResult::Ok("1");
        }

        DBResult DocEngine::UpdateMany(const std::string&               collection_name,
                                       const nexora::query::Condition&  condition,
                                       const nexora::query::UpdateSpec& update_spec) {
            if (!CollectionExists(collection_name))
                return DBResult::Err("Collection '" + collection_name + "' does not exist");

            std::string data_prefix = std::string(keys::kData) + collection_name + ":";
            rocksdb::ReadOptions ro;
            auto it = std::unique_ptr<rocksdb::Iterator>(txn_db_->NewIterator(ro));

            rocksdb::WriteBatch batch;
            uint64_t updated = 0;

            for (it->Seek(data_prefix);
                 it->Valid() && it->key().starts_with(data_prefix); it->Next()) {
                std::string bson = it->value().ToString();
                if (!MatchesCondition(bson, condition)) continue;
                std::string new_bson = ApplyUpdate(bson, update_spec);
                batch.Put(it->key(), new_bson);
                ++updated;
            }

            rocksdb::Status s = txn_db_->Write(write_options_, &batch);
            ROCKS_CHECK(s, "UpdateMany");
            return DBResult::Ok(std::to_string(updated));
        }

// ══════════════════════════════════════════════════════════════
// §12  CRUD - Delete
// ══════════════════════════════════════════════════════════════

        DBResult DocEngine::DeleteById(const std::string& collection_name,
                                       const std::string& doc_id) {
            std::string doc_key = MakeDocKey(collection_name, doc_id);
            std::string bson;
            rocksdb::Status s = txn_db_->Get(read_options_, doc_key, &bson);
            if (s.IsNotFound()) return DBResult::Ok("0");
            ROCKS_CHECK(s, "DeleteById read");

            CleanIndexesOnDelete(collection_name, doc_id, bson);

            s = txn_db_->Delete(write_options_, doc_key);
            ROCKS_CHECK(s, "DeleteById");

            // به‌روزرسانی شمارنده
            std::string seq_key = std::string(keys::kSeq) + collection_name;
            std::string seq_val;
            txn_db_->Get(read_options_, seq_key, &seq_val);
            if (!seq_val.empty()) {
                int64_t cnt = std::max(0LL, std::stoll(seq_val) - 1);
                txn_db_->Put(write_options_, seq_key, std::to_string(cnt));
            }

            return DBResult::Ok("1");
        }

        DBResult DocEngine::DeleteMany(const std::string&              collection_name,
                                       const nexora::query::Condition& condition) {
            if (!CollectionExists(collection_name))
                return DBResult::Err("Collection '" + collection_name + "' does not exist");

            std::string data_prefix = std::string(keys::kData) + collection_name + ":";
            rocksdb::ReadOptions ro;
            auto it = std::unique_ptr<rocksdb::Iterator>(txn_db_->NewIterator(ro));

            // جمع‌آوری اسناد هدف قبل از حذف
            std::vector<std::pair<std::string, std::string>> to_delete;
            for (it->Seek(data_prefix);
                 it->Valid() && it->key().starts_with(data_prefix); it->Next()) {
                std::string bson = it->value().ToString();
                if (MatchesCondition(bson, condition))
                    to_delete.emplace_back(it->key().ToString(), bson);
            }

            rocksdb::WriteBatch batch;
            for (const auto& [key, bson] : to_delete) {
                batch.Delete(key);
                CleanIndexesOnDelete(collection_name,
                                     key.substr(data_prefix.size()), bson);
            }

            rocksdb::Status s = txn_db_->Write(write_options_, &batch);
            ROCKS_CHECK(s, "DeleteMany");
            return DBResult::Ok(std::to_string(to_delete.size()));
        }

// ══════════════════════════════════════════════════════════════
// §13  Internal API برای GraphEngine
// ══════════════════════════════════════════════════════════════

        void DocEngine::IterateCollection(const std::string&      collection_name,
                                          const DocumentCallback& callback,
                                          uint32_t                batch_size) const {
            if (!CollectionExists(collection_name)) {
                NX_LOG("IterateCollection: '" << collection_name << "' not found");
                return;
            }

            std::string data_prefix = std::string(keys::kData) + collection_name + ":";

            // fill_cache=false مناسب bulk scan — مصرف cache RocksDB را کاهش می‌دهد
            rocksdb::ReadOptions ro;
            ro.fill_cache       = false;
            ro.total_order_seek = true;

            auto it = std::unique_ptr<rocksdb::Iterator>(txn_db_->NewIterator(ro));

            for (it->Seek(data_prefix);
                 it->Valid() && it->key().starts_with(data_prefix); it->Next()) {

                std::string doc_id    = it->key().ToString().substr(data_prefix.size());
                std::string bson_data = it->value().ToString();

                if (!callback(doc_id, bson_data)) break;  // early exit
            }

            // batch_size برای آینده (yield/throttle) نگه داشته شده
            (void)batch_size;
        }

        int64_t DocEngine::GetCollectionSize(const std::string& collection_name) const {
            std::string seq_key = std::string(keys::kSeq) + collection_name;
            std::string val;
            rocksdb::Status s = txn_db_->Get(read_options_, seq_key, &val);
            if (!s.ok() || val.empty()) return -1;
            try { return std::stoll(val); } catch (...) { return -1; }
        }

        std::vector<std::pair<std::string, std::string>>
        DocEngine::GetDocumentRange(const std::string& collection_name,
                                    const std::string& id_prefix,
                                    uint32_t           max_count) const {
            std::vector<std::pair<std::string, std::string>> results;
            std::string scan_prefix = std::string(keys::kData) + collection_name + ":";
            std::string seek_key    = scan_prefix + id_prefix;

            rocksdb::ReadOptions ro;
            ro.fill_cache = false;
            auto it = std::unique_ptr<rocksdb::Iterator>(txn_db_->NewIterator(ro));

            for (it->Seek(seek_key);
                 it->Valid() && it->key().starts_with(scan_prefix); it->Next()) {
                std::string doc_id = it->key().ToString().substr(scan_prefix.size());
                if (!id_prefix.empty() && doc_id.find(id_prefix) != 0) break;
                results.emplace_back(doc_id, it->value().ToString());
                if (max_count > 0 && results.size() >= max_count) break;
            }
            return results;
        }

// ══════════════════════════════════════════════════════════════
// §14  LookupJoin
// ══════════════════════════════════════════════════════════════

        JoinResult DocEngine::LookupJoin(const std::string&              from_collection,
                                         const std::string&              from_field,
                                         const std::string&              to_collection,
                                         const std::string&              to_field,
                                         const nexora::query::Condition& condition,
                                         uint32_t                        limit) {
            JoinResult result;

            if (!CollectionExists(from_collection)) {
                result.error_msg = "Collection '" + from_collection + "' does not exist";
                return result;
            }
            if (!CollectionExists(to_collection)) {
                result.error_msg = "Collection '" + to_collection + "' does not exist";
                return result;
            }

            std::string data_prefix = std::string(keys::kData) + from_collection + ":";
            rocksdb::ReadOptions ro;
            auto it = std::unique_ptr<rocksdb::Iterator>(txn_db_->NewIterator(ro));
            uint32_t count = 0;

            for (it->Seek(data_prefix);
                 it->Valid() && it->key().starts_with(data_prefix); it->Next()) {

                std::string from_bson = it->value().ToString();
                if (!MatchesCondition(from_bson, condition)) continue;

                std::string join_val = ExtractField(from_bson, from_field);
                if (join_val.empty()) continue;

                std::string to_bson;
                if (to_field == "_id") {
                    rocksdb::Status s = txn_db_->Get(read_options_,
                                                     MakeDocKey(to_collection, join_val),
                                                     &to_bson);
                    if (!s.ok()) continue;
                } else {
                    std::string idx_prefix = std::string(keys::kIdx) + to_collection +
                                             ":" + to_field + ":" + join_val + ":";
                    rocksdb::ReadOptions ro2;
                    auto it2 = std::unique_ptr<rocksdb::Iterator>(txn_db_->NewIterator(ro2));
                    it2->Seek(idx_prefix);
                    if (!it2->Valid() || !it2->key().starts_with(idx_prefix)) continue;
                    std::string ref_id = it2->value().ToString();
                    rocksdb::Status s = txn_db_->Get(read_options_,
                                                     MakeDocKey(to_collection, ref_id),
                                                     &to_bson);
                    if (!s.ok()) continue;
                }

                // Merge: from_bson + field "__joined__"
                std::string merged = from_bson;
                if (!merged.empty() && merged.back() == '}') {
                    merged.pop_back();
                    merged += ",\"__joined__\":" + to_bson + "}";
                }
                result.records.push_back(std::move(merged));
                ++count;
                if (limit > 0 && count >= limit) break;
            }

            result.success = true;
            return result;
        }

// ══════════════════════════════════════════════════════════════
// §15  مدیریت Transaction
// ══════════════════════════════════════════════════════════════

        std::unique_ptr<TxHandle> DocEngine::BeginTransaction() {
            rocksdb::WriteOptions wo;
            wo.sync = false;
            rocksdb::Transaction* tx = txn_db_->BeginTransaction(wo);
            if (!tx) {
                NX_LOG("BeginTransaction: failed");
                return nullptr;
            }
            return std::make_unique<TxHandle>(tx);
        }

        DBResult DocEngine::CommitTransaction(TxHandle& tx_handle) {
            if (!tx_handle.IsValid())
                return DBResult::Err("Invalid transaction handle");
            rocksdb::Status s = tx_handle.Get()->Commit();
            ROCKS_CHECK(s, "CommitTransaction");
            return DBResult::Ok("Transaction committed");
        }

        DBResult DocEngine::RollbackTransaction(TxHandle& tx_handle) {
            if (!tx_handle.IsValid())
                return DBResult::Err("Invalid transaction handle");
            rocksdb::Status s = tx_handle.Get()->Rollback();
            ROCKS_CHECK(s, "RollbackTransaction");
            return DBResult::Ok("Transaction rolled back");
        }

        DBResult DocEngine::InsertOneTx(TxHandle&          tx_handle,
                                        const std::string& collection_name,
                                        const std::string& bson_document) {
            if (!tx_handle.IsValid()) return DBResult::Err("Invalid transaction");
            if (!CollectionExists(collection_name))
                return DBResult::Err("Collection '" + collection_name + "' does not exist");

            std::string doc_id = ExtractField(bson_document, "_id");
            if (doc_id.empty()) doc_id = GenerateDocId();

            rocksdb::Status s = tx_handle.Get()->Put(
                    MakeDocKey(collection_name, doc_id), bson_document);
            ROCKS_CHECK(s, "InsertOneTx");
            return DBResult::Ok(doc_id);
        }

        DBResult DocEngine::UpdateByIdTx(TxHandle&                        tx_handle,
                                         const std::string&               collection_name,
                                         const std::string&               doc_id,
                                         const nexora::query::UpdateSpec& update_spec) {
            if (!tx_handle.IsValid()) return DBResult::Err("Invalid transaction");

            std::string doc_key = MakeDocKey(collection_name, doc_id);
            std::string bson;
            rocksdb::Status s = tx_handle.Get()->GetForUpdate(read_options_, doc_key, &bson);
            if (s.IsNotFound()) return DBResult::Err("Document not found: " + doc_id);
            ROCKS_CHECK(s, "UpdateByIdTx read");

            std::string new_bson = ApplyUpdate(bson, update_spec);
            s = tx_handle.Get()->Put(doc_key, new_bson);
            ROCKS_CHECK(s, "UpdateByIdTx write");
            return DBResult::Ok("1");
        }

        DBResult DocEngine::DeleteByIdTx(TxHandle&          tx_handle,
                                         const std::string& collection_name,
                                         const std::string& doc_id) {
            if (!tx_handle.IsValid()) return DBResult::Err("Invalid transaction");

            std::string doc_key = MakeDocKey(collection_name, doc_id);
            std::string bson;
            rocksdb::Status s = tx_handle.Get()->GetForUpdate(read_options_, doc_key, &bson);
            if (s.IsNotFound()) return DBResult::Ok("0");
            ROCKS_CHECK(s, "DeleteByIdTx read");

            s = tx_handle.Get()->Delete(doc_key);
            ROCKS_CHECK(s, "DeleteByIdTx delete");
            return DBResult::Ok("1");
        }

        DBResult DocEngine::FindByIdTx(TxHandle&          tx_handle,
                                       const std::string& collection_name,
                                       const std::string& doc_id) {
            if (!tx_handle.IsValid()) return DBResult::Err("Invalid transaction");

            std::string doc_key = MakeDocKey(collection_name, doc_id);
            std::string bson;
            rocksdb::Status s = tx_handle.Get()->Get(read_options_, doc_key, &bson);
            if (s.IsNotFound()) return DBResult::Err("Document not found: " + doc_id);
            ROCKS_CHECK(s, "FindByIdTx");
            return DBResult::Ok(bson);
        }

// ══════════════════════════════════════════════════════════════
// §16  توابع کمکی
// ══════════════════════════════════════════════════════════════

        DBResult DocEngine::Count(const std::string&              collection_name,
                                  const nexora::query::Condition& condition) {
            if (!CollectionExists(collection_name))
                return DBResult::Err("Collection '" + collection_name + "' does not exist");

            // O(1) اگر شرط خالی باشد
            if (condition.IsEmpty()) {
                int64_t sz = GetCollectionSize(collection_name);
                return DBResult::Ok(std::to_string(sz >= 0 ? sz : 0));
            }

            std::string data_prefix = std::string(keys::kData) + collection_name + ":";
            rocksdb::ReadOptions ro;
            auto it = std::unique_ptr<rocksdb::Iterator>(txn_db_->NewIterator(ro));
            int64_t count = 0;
            for (it->Seek(data_prefix);
                 it->Valid() && it->key().starts_with(data_prefix); it->Next())
                if (MatchesCondition(it->value().ToString(), condition)) ++count;

            return DBResult::Ok(std::to_string(count));
        }

        DBResult DocEngine::Exists(const std::string&              collection_name,
                                   const nexora::query::Condition& condition) {
            if (!CollectionExists(collection_name))
                return DBResult::Err("Collection '" + collection_name + "' does not exist");

            std::string data_prefix = std::string(keys::kData) + collection_name + ":";
            rocksdb::ReadOptions ro;
            auto it = std::unique_ptr<rocksdb::Iterator>(txn_db_->NewIterator(ro));
            for (it->Seek(data_prefix);
                 it->Valid() && it->key().starts_with(data_prefix); it->Next())
                if (MatchesCondition(it->value().ToString(), condition))
                    return DBResult::Ok("true");

            return DBResult::Ok("false");
        }

// ══════════════════════════════════════════════════════════════
// §17  دسترسی به پیکربندی
// ══════════════════════════════════════════════════════════════

        std::optional<SchemaDefinition>
        DocEngine::GetSchema(const std::string& collection_name) const {
            std::string val;
            rocksdb::Status s = txn_db_->Get(read_options_,
                                             MakeMetaKey(keys::kMetaCol, collection_name),
                                             &val);
            if (!s.ok()) return std::nullopt;
            return DeserializeSchema(val);
        }

        std::vector<ForeignKeyDefinition>
        DocEngine::GetForeignKeys(const std::string& collection_name) const {
            std::vector<ForeignKeyDefinition> result;
            std::string prefix = std::string(keys::kMetaFk) + collection_name + ":";
            rocksdb::ReadOptions ro;
            auto it = std::unique_ptr<rocksdb::Iterator>(txn_db_->NewIterator(ro));
            for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
                auto fk = DeserializeFk(it->value().ToString());
                if (fk) result.push_back(*fk);
            }
            return result;
        }

        std::vector<IndexDefinition>
        DocEngine::GetIndexes(const std::string& collection_name) const {
            std::vector<IndexDefinition> result;
            std::string prefix = std::string(keys::kMetaIdx) + collection_name + ":";
            rocksdb::ReadOptions ro;
            auto it = std::unique_ptr<rocksdb::Iterator>(txn_db_->NewIterator(ro));
            for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
                auto idx = DeserializeIndex(it->value().ToString());
                if (idx) result.push_back(*idx);
            }
            return result;
        }

        std::vector<std::string> DocEngine::ListCollections() const {
            std::vector<std::string> result;
            std::string prefix = keys::kMetaCol;
            rocksdb::ReadOptions ro;
            auto it = std::unique_ptr<rocksdb::Iterator>(txn_db_->NewIterator(ro));
            for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next())
                result.push_back(it->key().ToString().substr(prefix.size()));
            return result;
        }

    } // namespace core
} // namespace nexora