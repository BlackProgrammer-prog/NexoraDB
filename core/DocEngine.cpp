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
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <limits>
#include <stdexcept>

#if defined(__linux__)
#include <unistd.h>
#endif

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
            constexpr char kReservedPrefix[] = "__nexora_";
            constexpr char kInternalUsers[]  = "__nexora_internal_users";
            constexpr char kInternalAppTokens[] = "__nexora_internal_app_tokens";
        } // namespace keys



// ══════════════════════════════════════════════════════════════
        namespace {
            DBResult RocksFailure(
                    const std::string& operation,
                    const rocksdb::Status& status
                    ) {
                std::string category;
                if (status.IsTimedOut()) {
                    category = "[transaction-timeout] ";
                } else if (status.IsDeadlock()) {
                    category = "[transaction-deadlock] ";
                } else if (status.IsBusy()) {
                    category = "[transaction-conflict] ";
                }
                return DBResult::Err(
                        category + "[RocksDB] " + operation + ": " +
                        status.ToString());
            }
        } //namespace


// ══════════════════════════════════════════════════════════════



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

        bool DocEngine::IsReservedCollectionName(const std::string& name) {
            return name.starts_with(keys::kReservedPrefix);
        }

        bool DocEngine::EnsureInternalCollections() {
            rocksdb::WriteBatch batch;
            for (const std::string& collection : {
                     std::string(keys::kInternalUsers),
                     std::string(keys::kInternalAppTokens)}) {
                const std::string meta_key = MakeMetaKey(keys::kMetaCol, collection);
                std::string existing;
                rocksdb::Status s = txn_db_->Get(read_options_, meta_key, &existing);
                if (!s.ok() && !s.IsNotFound()) return false;
                if (s.IsNotFound())
                    batch.Put(meta_key, SerializeSchema(SchemaDefinition{}));

                const std::string seq_key = std::string(keys::kSeq) + collection;
                std::string seq_val;
                s = txn_db_->Get(read_options_, seq_key, &seq_val);
                if (!s.ok() && !s.IsNotFound()) return false;
                if (s.IsNotFound()) batch.Put(seq_key, "0");
            }

            if (batch.Count() == 0) return true;
            return txn_db_->Write(write_options_, &batch).ok();
        }

        bool DocEngine::ValidateInternalUserDocument(const std::string& user_json,
                                                     std::string&       username_out,
                                                     std::string&       error_out) const {
            username_out = ExtractField(user_json, "username");
            if (username_out.empty() || username_out == "null") {
                error_out = "username is required";
                return false;
            }

            const std::string password_hash = ExtractField(user_json, "password_hash");
            if (password_hash.empty() || password_hash == "null") {
                error_out = "password_hash is required";
                return false;
            }

            const bool argon2id = password_hash.starts_with("$argon2id$");
            const bool bcrypt = password_hash.starts_with("$2a$") ||
                                password_hash.starts_with("$2b$") ||
                                password_hash.starts_with("$2y$");
            if (!argon2id && !bcrypt) {
                error_out = "password_hash must be argon2id or bcrypt";
                return false;
            }

            const std::string role = ExtractField(user_json, "role");
            if (role != "admin" && role != "application") {
                error_out = "invalid role";
                return false;
            }

            const std::string status = ExtractField(user_json, "status");
            if (!status.empty() && status != "active" &&
                status != "disabled" && status != "deleted") {
                error_out = "invalid status";
                return false;
            }

            if (role == "admin") {
                const std::string email = ExtractField(user_json, "email");
                const std::string first_name = ExtractField(user_json, "first_name");
                const std::string last_name = ExtractField(user_json, "last_name");
                if (email.empty() || email == "null" ||
                    first_name.empty() || first_name == "null" ||
                    last_name.empty() || last_name == "null") {
                    error_out = "admin requires email, first_name and last_name";
                    return false;
                }
            }

            return true;
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

        DocEngine::DocEngine(
                const std::string& db_path,
                const TransactionSettings& transaction_settings)
                : db_path_(db_path),
                  txn_db_(nullptr, [](rocksdb::TransactionDB* db) {
                      if (db) { db->SyncWAL(); delete db; }
                  }) {

            db_options_.create_if_missing              = true;
            db_options_.create_missing_column_families = true;
            db_options_.compression                    = rocksdb::kSnappyCompression;
            db_options_.max_open_files                 = 500;
            db_options_.write_buffer_size             = 64 * 1024 * 1024;  // 64 MB
            db_options_.max_write_buffer_number       = 3;
            db_options_.target_file_size_base         = 64 * 1024 * 1024;

            write_options_.sync         = false;  // برای performance
            read_options_.verify_checksums = true;

            txn_db_options_.transaction_lock_timeout =
                    transaction_settings.lock_timeout_ms;
            txn_db_options_.default_lock_timeout =
                    transaction_settings.lock_timeout_ms;
            transaction_options_.lock_timeout =
                    transaction_settings.lock_timeout_ms;
            transaction_options_.expiration =
                    transaction_settings.expiration_ms;
            transaction_options_.deadlock_detect =
                    transaction_settings.deadlock_detect;
            transaction_options_.deadlock_detect_depth =
                    transaction_settings.deadlock_detect_depth;

            rocksdb::TransactionDB* raw_db = nullptr;
            rocksdb::Status s = rocksdb::TransactionDB::Open(
                    db_options_, txn_db_options_, db_path_, &raw_db);

            if (!s.ok()) {
                throw std::runtime_error("[NexoraDB] Failed to open database at '" +
                                         db_path_ + "': " + s.ToString());
            }

            txn_db_.reset(raw_db);
            if (!EnsureInternalCollections()) {
                throw std::runtime_error("[NexoraDB] Failed to initialize internal collections");
            }

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

        void DocEngine::SetMutationFaultPointForTesting(
                MutationFaultPoint point) noexcept {
            mutation_fault_point_.store(point, std::memory_order_release);
        }

        rocksdb::Status DocEngine::InjectMutationFaultIfRequested(
                MutationFaultPoint point) noexcept {
            MutationFaultPoint expected = point;
            if (mutation_fault_point_.compare_exchange_strong(
                    expected,
                    MutationFaultPoint::None,
                    std::memory_order_acq_rel)) {
                return rocksdb::Status::IOError(
                        "Injected mutation fault at stage " +
                        std::to_string(static_cast<unsigned>(point)));
            }
            return rocksdb::Status::OK();
        }

// ══════════════════════════════════════════════════════════════
// §5  مدیریت Collection و Schema
// ══════════════════════════════════════════════════════════════

        DBResult DocEngine::CreateCollection(
                const std::string& collection_name,
                const std::optional<SchemaDefinition>& schema) {

            if (collection_name.empty())
                return DBResult::Err("Collection name cannot be empty");
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");

            std::string meta_key = MakeMetaKey(keys::kMetaCol, collection_name);
            std::string existing;
            rocksdb::Status s = txn_db_->Get(read_options_, meta_key, &existing);

            if (s.ok())
                return DBResult::Err("Collection '" + collection_name + "' already exists");
            if (!s.IsNotFound())
                return DBResult::Err("RocksDB error: " + s.ToString());

            SchemaDefinition eff_schema = schema.value_or(SchemaDefinition{});
            rocksdb::WriteBatch batch;
            batch.Put(meta_key, SerializeSchema(eff_schema));
            batch.Put(std::string(keys::kSeq) + collection_name, "0");
            s = txn_db_->Write(write_options_, &batch);
            ROCKS_CHECK(s, "CreateCollection metadata/counter");

            NX_LOG("Collection created: " << collection_name);
            return DBResult::Ok("Collection '" + collection_name + "' created");
        }

        DBResult DocEngine::DropCollection(const std::string& collection_name) {
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");
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

            if (!it->status().ok())
                return RocksFailure("DropCollection scan", it->status());

            batch.Delete(MakeMetaKey(keys::kMetaCol, collection_name));
            batch.Delete(std::string(keys::kSeq) + collection_name);

            rocksdb::Status s = txn_db_->Write(write_options_, &batch);
            ROCKS_CHECK(s, "DropCollection");

            NX_LOG("Collection dropped: " << collection_name);
            return DBResult::Ok("Collection '" + collection_name + "' dropped");
        }

        DBResult DocEngine::SetSchemaValidation(const std::string&      collection_name,
                                                const SchemaDefinition& schema) {
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");
            if (!CollectionExists(collection_name))
                return DBResult::Err("Collection '" + collection_name + "' does not exist");
            rocksdb::Status s = txn_db_->Put(write_options_,
                                             MakeMetaKey(keys::kMetaCol, collection_name),
                                             SerializeSchema(schema));
            ROCKS_CHECK(s, "SetSchemaValidation");
            return DBResult::Ok("Schema updated for '" + collection_name + "'");
        }

        bool DocEngine::CollectionExists(const std::string& collection_name) const {
            if (IsReservedCollectionName(collection_name)) return false;
            std::string val;
            return txn_db_->Get(read_options_,
                                MakeMetaKey(keys::kMetaCol, collection_name), &val).ok();
        }

// ══════════════════════════════════════════════════════════════
// §6  مدیریت Index
// ══════════════════════════════════════════════════════════════

        DBResult DocEngine::CreateIndex(const std::string&     collection_name,
                                        const IndexDefinition& index_def) {
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");
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
            if (!s.IsNotFound())
                return RocksFailure("CreateIndex metadata lookup", s);

            // Rebuild index از داده‌های موجود
            std::string data_prefix = std::string(keys::kData) + collection_name + ":";
            rocksdb::ReadOptions ro;
            auto it = std::unique_ptr<rocksdb::Iterator>(txn_db_->NewIterator(ro));
            rocksdb::WriteBatch batch;
            batch.Put(meta_key, SerializeIndex(index_def));
            uint64_t cnt = 0;

            for (it->Seek(data_prefix);
                 it->Valid() &&
                 it->key().starts_with(rocksdb::Slice(data_prefix));
                 it->Next()) {
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

            if (!it->status().ok())
                return RocksFailure("CreateIndex data scan", it->status());

            s = txn_db_->Write(write_options_, &batch);
            ROCKS_CHECK(s, "CreateIndex atomic metadata/rebuild");

            return DBResult::Ok("Index '" + index_def.index_name +
                                "' created (" + std::to_string(cnt) + " entries)");
        }

        DBResult DocEngine::DropIndex(const std::string& collection_name,
                                      const std::string& index_name) {
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");
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
            if (!it->status().ok())
                return RocksFailure("DropIndex scan", it->status());
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
            if (IsReservedCollectionName(collection_name) ||
                IsReservedCollectionName(fk_def.ref_collection))
                return DBResult::Err("reserved collection name");
            if (!CollectionExists(collection_name))
                return DBResult::Err("Collection '" + collection_name + "' does not exist");
            if (!CollectionExists(fk_def.ref_collection))
                return DBResult::Err("Referenced collection '" +
                                     fk_def.ref_collection + "' does not exist");
            if (fk_def.fk_name.empty() ||
                fk_def.local_field.empty() ||
                fk_def.ref_collection.empty() ||
                fk_def.ref_field.empty())
                return DBResult::Err("Foreign key definition is incomplete");

            if (fk_def.ref_field != "_id") {
                MutationMetadata referenced_metadata;
                rocksdb::Status metadata_status = LoadMutationMetadata(
                        fk_def.ref_collection,
                        referenced_metadata);
                if (!metadata_status.ok())
                    return RocksFailure(
                            "AddForeignKey referenced metadata",
                            metadata_status);

                const bool has_supporting_index = std::any_of(
                        referenced_metadata.indexes.begin(),
                        referenced_metadata.indexes.end(),
                        [&](const IndexDefinition& index) {
                            return std::find(
                                    index.fields.begin(),
                                    index.fields.end(),
                                    fk_def.ref_field) != index.fields.end();
                        });

                if (!has_supporting_index) {
                    return DBResult::Err(
                            "Foreign key reference field '" +
                            fk_def.ref_collection + "." +
                            fk_def.ref_field +
                            "' requires an index");
                }
            }

            std::string meta_key = MakeMetaKey(keys::kMetaFk, collection_name, fk_def.fk_name);
            std::string ex;
            rocksdb::Status s = txn_db_->Get(read_options_, meta_key, &ex);
            if (s.ok())
                return DBResult::Err("Foreign key '" + fk_def.fk_name + "' already exists");
            if (!s.IsNotFound())
                return RocksFailure("AddForeignKey metadata lookup", s);

            s = txn_db_->Put(write_options_, meta_key, SerializeFk(fk_def));
            ROCKS_CHECK(s, "AddForeignKey");
            return DBResult::Ok("Foreign key '" + fk_def.fk_name + "' added");
        }

        DBResult DocEngine::DropForeignKey(const std::string& collection_name,
                                           const std::string& fk_name) {
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");
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

        rocksdb::Status DocEngine::LoadMutationMetadata(const std::string &collection_name,
                                                        nexora::core::DocEngine::MutationMetadata &metadata) const {
            metadata = MutationMetadata{};

            std::string serialized_schema;

            rocksdb::Status status = txn_db_->Get(
                    read_options_,
                    MakeMetaKey(keys::kMetaCol, collection_name),
                    &serialized_schema
            );

            if (!status.ok()) {
                return status;
            }

            const auto schema = DeserializeSchema(serialized_schema);

            if (!schema.has_value()) {
                return rocksdb::Status::Corruption(
                        "Invalid schema metadata for collection: " +
                        collection_name
                );
            }
            metadata.schema = *schema;

            const std::string foreign_key_prefix = std::string(keys::kMetaFk) + collection_name + ":";

            rocksdb::ReadOptions iterator_options = read_options_;
            {
                auto iterator = std::unique_ptr<rocksdb::Iterator>(
                        txn_db_->NewIterator(iterator_options)
                );

                for (iterator->Seek(foreign_key_prefix);
                     iterator->Valid() &&
                     iterator->key().starts_with(foreign_key_prefix);
                     iterator->Next()) {
                    const auto foreign_key =
                            DeserializeFk(iterator->value().ToString());

                    if (!foreign_key.has_value()) {
                        return rocksdb::Status::Corruption(
                                "Invalid foreign-key metadata in collection: " +
                                collection_name);
                    }

                    metadata.foreign_keys.push_back(*foreign_key);
                }
                status = iterator -> status();

                if (!status.ok()){
                    return status;
                }
            }
            const std::string index_prefix = std::string(keys::kMetaIdx) + collection_name + ":" ;

            {
                auto iterator = std::unique_ptr<rocksdb::Iterator>(
                        txn_db_->NewIterator(iterator_options));

                for (iterator->Seek(index_prefix);
                     iterator->Valid() &&
                     iterator->key().starts_with(index_prefix);
                     iterator->Next()) {
                    const auto index =
                            DeserializeIndex(iterator->value().ToString());

                    if (!index.has_value()) {
                        return rocksdb::Status::Corruption(
                                "Invalid index metadata in collection: " +
                                collection_name);
                    }

                    metadata.indexes.push_back(*index);
                }

                status = iterator->status();

                if (!status.ok()) {
                    return status;
                }
            }
            return rocksdb::Status::OK();
        }

        rocksdb::Status DocEngine::ValidateForeignKeysChecked(
                const std::vector<ForeignKeyDefinition>& foreign_keys,
                const std::string& bson_document,
                std::string& validation_error,
                rocksdb::Transaction* transaction) const {
            validation_error.clear();
            std::unique_ptr<rocksdb::Iterator> index_iterator;

            for (const auto& foreign_key : foreign_keys) {
                const std::string local_value =
                        ExtractField(bson_document, foreign_key.local_field);

                if (local_value.empty()) {
                    continue;
                }

                if (foreign_key.ref_field == "_id") {
                    std::string referenced_document;

                    const std::string referenced_key = MakeDocKey(
                            foreign_key.ref_collection,
                            local_value);
                    const rocksdb::Status status = transaction
                            ? transaction->Get(
                                    read_options_,
                                    referenced_key,
                                    &referenced_document)
                            : txn_db_->Get(
                                    read_options_,
                                    referenced_key,
                                    &referenced_document);

                    if (status.IsNotFound()) {
                        validation_error =
                                "FK violation: '" + local_value +
                                "' not found in '" +
                                foreign_key.ref_collection + "'";

                        return rocksdb::Status::InvalidArgument(
                                validation_error);
                    }

                    if (!status.ok()) {
                        return status;
                    }

                    continue;
                }

                const std::string index_prefix =
                        std::string(keys::kIdx) +
                        foreign_key.ref_collection + ":" +
                        foreign_key.ref_field + ":" +
                        local_value + ":";

                if (!index_iterator) {
                    index_iterator.reset(transaction
                            ? transaction->GetIterator(read_options_)
                            : txn_db_->NewIterator(read_options_));
                }

                index_iterator->Seek(index_prefix);

                if (!index_iterator->status().ok()) {
                    return index_iterator->status();
                }

                const rocksdb::Slice prefix_slice(index_prefix);
                if (!index_iterator->Valid() ||
                    !index_iterator->key().starts_with(prefix_slice)) {
                    validation_error =
                            "FK violation: '" + local_value +
                            "' not found in '" +
                            foreign_key.ref_collection + "." +
                            foreign_key.ref_field + "'";

                    return rocksdb::Status::InvalidArgument(
                            validation_error);
                }
            }

            return rocksdb::Status::OK();
        }

        class DocEngine::MutationBuilder {
        public:
            MutationBuilder(
                    DocEngine& engine,
                    const std::string& collection,
                    const std::vector<IndexDefinition>& indexes)
                    : engine_(engine),
                      collection_(collection),
                      indexes_(indexes) {
                owned_transaction_.reset(
                        engine_.txn_db_->BeginTransaction(
                                engine_.write_options_,
                                engine_.transaction_options_));
                transaction_ = owned_transaction_.get();
            }

            MutationBuilder(
                    DocEngine& engine,
                    rocksdb::Transaction& transaction,
                    const std::string& collection,
                    const std::vector<IndexDefinition>& indexes)
                    : engine_(engine),
                      collection_(collection),
                      indexes_(indexes),
                      transaction_(&transaction),
                      uses_savepoint_(true) {
                transaction_->SetSavePoint();
            }

            ~MutationBuilder() {
                if (transaction_ && !finished_) {
                    const rocksdb::Status rollback_status = uses_savepoint_
                            ? transaction_->RollbackToSavePoint()
                            : transaction_->Rollback();

                    if (!rollback_status.ok()) {
                        NX_LOG(
                                "Mutation rollback failed: "
                                        << rollback_status.ToString());
                    }
                }
            }

            MutationBuilder(const MutationBuilder&) = delete;
            MutationBuilder& operator=(const MutationBuilder&) = delete;

            bool IsValid() const noexcept {
                return transaction_ != nullptr;
            }

            rocksdb::Status ReadDocumentForUpdate(
                    const std::string& document_id,
                    std::string& document) {
                if (!transaction_) {
                    return rocksdb::Status::InvalidArgument(
                            "Mutation transaction is not initialized");
                }

                return transaction_->GetForUpdate(
                        engine_.read_options_,
                        DocEngine::MakeDocKey(
                                collection_,
                                document_id),
                        &document);
            }

            rocksdb::Status PutDocument(
                    const std::string& document_id,
                    const std::string& bson_document) {
                rocksdb::Status status = transaction_->Put(
                        DocEngine::MakeDocKey(
                                collection_,
                                document_id),
                        bson_document);

                if (!status.ok()) {
                    return status;
                }

                status = engine_.InjectMutationFaultIfRequested(
                        MutationFaultPoint::AfterDocument);
                if (!status.ok()) return status;

                status = PutIndexEntries(
                        document_id,
                        bson_document);
                if (!status.ok()) return status;

                return engine_.InjectMutationFaultIfRequested(
                        MutationFaultPoint::AfterIndexes);
            }

            rocksdb::Status ReplaceDocument(
                    const std::string& document_id,
                    const std::string& old_document,
                    const std::string& new_document) {
                rocksdb::Status status = DeleteIndexEntries(
                        document_id,
                        old_document);

                if (!status.ok()) {
                    return status;
                }

                status = transaction_->Put(
                        DocEngine::MakeDocKey(
                                collection_,
                                document_id),
                        new_document);

                if (!status.ok()) {
                    return status;
                }

                return PutIndexEntries(
                        document_id,
                        new_document);
            }

            rocksdb::Status DeleteDocument(
                    const std::string& document_id,
                    const std::string& old_document) {
                rocksdb::Status status = DeleteIndexEntries(
                        document_id,
                        old_document);

                if (!status.ok()) {
                    return status;
                }

                return transaction_->Delete(
                        DocEngine::MakeDocKey(
                                collection_,
                                document_id));
            }

            rocksdb::Status AdjustCounter(std::int64_t delta) {
                const std::string counter_key =
                        std::string(keys::kSeq) + collection_;

                std::string stored_value;

                rocksdb::Status status = transaction_->GetForUpdate(
                        engine_.read_options_,
                        counter_key,
                        &stored_value);

                if (status.IsNotFound()) {
                    return rocksdb::Status::Corruption(
                            "Missing document counter for collection: " +
                            collection_);
                }

                if (!status.ok()) {
                    return status;
                }

                std::size_t parsed_characters = 0;
                std::int64_t current_value = 0;

                try {
                    current_value = std::stoll(
                            stored_value,
                            &parsed_characters);
                } catch (const std::exception&) {
                    return rocksdb::Status::Corruption(
                            "Invalid document counter for collection: " +
                            collection_);
                }

                if (parsed_characters != stored_value.size()) {
                    return rocksdb::Status::Corruption(
                            "Invalid document counter for collection: " +
                            collection_);
                }

                if (delta > 0 &&
                    current_value >
                    std::numeric_limits<std::int64_t>::max() - delta) {
                    return rocksdb::Status::InvalidArgument(
                            "Document counter overflow");
                }

                std::int64_t next_value = current_value + delta;

                if (next_value < 0) {
                    next_value = 0;
                }

                status = transaction_->Put(
                        counter_key,
                        std::to_string(next_value));
                if (!status.ok()) return status;

                return engine_.InjectMutationFaultIfRequested(
                        MutationFaultPoint::AfterCounter);
            }

            std::unique_ptr<rocksdb::Iterator> NewIterator(
                    const rocksdb::ReadOptions& options) {
                return std::unique_ptr<rocksdb::Iterator>(
                        transaction_->GetIterator(options));
            }

            rocksdb::Status Commit() {
                if (!transaction_) {
                    return rocksdb::Status::InvalidArgument(
                            "Mutation transaction is not initialized");
                }

                const rocksdb::Status status = uses_savepoint_
                        ? transaction_->PopSavePoint()
                        : transaction_->Commit();

                if (status.ok()) {
                    finished_ = true;
                }

                return status;
            }

            rocksdb::Status Rollback() {
                if (!transaction_) {
                    return rocksdb::Status::InvalidArgument(
                            "Mutation transaction is not initialized");
                }

                const rocksdb::Status status = uses_savepoint_
                        ? transaction_->RollbackToSavePoint()
                        : transaction_->Rollback();
                if (status.ok()) {
                    finished_ = true;
                }
                return status;
            }

        private:
            rocksdb::Status PutIndexEntries(
                    const std::string& document_id,
                    const std::string& bson_document) {
                for (const auto& index : indexes_) {
                    for (const auto& field : index.fields) {
                        const std::string value =
                                DocEngine::ExtractField(
                                        bson_document,
                                        field);

                        if (value.empty()) {
                            continue;
                        }

                        const rocksdb::Status status =
                                transaction_->Put(
                                        DocEngine::MakeIndexKey(
                                                collection_,
                                                field,
                                                value,
                                                document_id),
                                        document_id);

                        if (!status.ok()) {
                            return status;
                        }
                    }
                }

                return rocksdb::Status::OK();
            }

            rocksdb::Status DeleteIndexEntries(
                    const std::string& document_id,
                    const std::string& bson_document) {
                for (const auto& index : indexes_) {
                    for (const auto& field : index.fields) {
                        const std::string value =
                                DocEngine::ExtractField(
                                        bson_document,
                                        field);

                        if (value.empty()) {
                            continue;
                        }

                        const rocksdb::Status status =
                                transaction_->Delete(
                                        DocEngine::MakeIndexKey(
                                                collection_,
                                                field,
                                                value,
                                                document_id));

                        if (!status.ok()) {
                            return status;
                        }
                    }
                }

                return rocksdb::Status::OK();
            }

            DocEngine& engine_;
            std::string collection_;
            const std::vector<IndexDefinition>& indexes_;
            std::unique_ptr<rocksdb::Transaction> owned_transaction_;
            rocksdb::Transaction* transaction_ = nullptr;
            bool uses_savepoint_ = false;
            bool finished_ = false;
        };
// ══════════════════════════════════════════════════════════════

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

// ══════════════════════════════════════════════════════════════
// §9  CRUD - Insert
// ══════════════════════════════════════════════════════════════

        DBResult DocEngine::InsertOne(
                const std::string& collection_name,
                const std::string& bson_document) {
            if (IsReservedCollectionName(collection_name)) {
                return DBResult::Err("reserved collection name");
            }

            MutationMetadata metadata;

            rocksdb::Status status =
                    LoadMutationMetadata(
                            collection_name,
                            metadata);

            if (status.IsNotFound()) {
                return DBResult::Err(
                        "Collection '" + collection_name +
                        "' does not exist");
            }

            if (!status.ok()) {
                return RocksFailure(
                        "InsertOne metadata",
                        status);
            }

            if (!metadata.schema.fields.empty()) {
                std::string validation_error;

                if (!ValidateDocument(
                        bson_document,
                        metadata.schema,
                        validation_error)) {
                    return DBResult::Err(
                            "Schema validation failed: " +
                            validation_error);
                }
            }

            std::string validation_error;

            status = ValidateForeignKeysChecked(
                    metadata.foreign_keys,
                    bson_document,
                    validation_error);

            if (!status.ok()) {
                if (!validation_error.empty()) {
                    return DBResult::Err(validation_error);
                }

                return RocksFailure(
                        "InsertOne foreign-key validation",
                        status);
            }

            std::string document_id =
                    ExtractField(bson_document, "_id");

            if (document_id.empty()) {
                document_id = GenerateDocId();
            }

            MutationBuilder mutation(
                    *this,
                    collection_name,
                    metadata.indexes);

            if (!mutation.IsValid()) {
                return DBResult::Err(
                        "InsertOne: unable to start transaction");
            }

            std::string old_document;

            status = mutation.ReadDocumentForUpdate(
                    document_id,
                    old_document);

            if (status.ok()) {
                return DBResult::Err(
                        "Duplicate document _id: " + document_id);
            }

            if (!status.IsNotFound()) {
                return RocksFailure(
                        "InsertOne document lookup",
                        status);
            }

            status = mutation.PutDocument(
                    document_id,
                    bson_document);

            if (!status.ok()) {
                return RocksFailure(
                        "InsertOne document/index staging",
                        status);
            }

            status = mutation.AdjustCounter(1);

            if (!status.ok()) {
                return RocksFailure(
                        "InsertOne counter staging",
                        status);
            }

            status = mutation.Commit();

            if (!status.ok()) {
                return RocksFailure(
                        "InsertOne commit",
                        status);
            }

            return DBResult::Ok(document_id);
        }

        DBResult DocEngine::InsertMany(const std::string&              collection_name,
                                       const std::vector<std::string>& bson_documents) {
            if (IsReservedCollectionName(collection_name)) {
                return DBResult::Err("reserved collection name");
            }
            if (bson_documents.empty()) {
                return DBResult::Err("Document list is empty");
            }

            MutationMetadata metadata;
            rocksdb::Status status = LoadMutationMetadata(
                    collection_name,
                    metadata);

            if (status.IsNotFound()) {
                return DBResult::Err(
                        "Collection '" + collection_name +
                        "' does not exist");
            }
            if (!status.ok()) {
                return RocksFailure("InsertMany metadata", status);
            }

            MutationBuilder mutation(
                    *this,
                    collection_name,
                    metadata.indexes);
            if (!mutation.IsValid()) {
                return DBResult::Err(
                        "InsertMany: unable to start transaction");
            }

            std::vector<std::string> created_ids;
            created_ids.reserve(bson_documents.size());

            for (const auto& bson : bson_documents) {
                if (!metadata.schema.fields.empty()) {
                    std::string validation_error;
                    if (!ValidateDocument(
                            bson,
                            metadata.schema,
                            validation_error)) {
                        return DBResult::Err(
                                "Schema validation failed: " +
                                validation_error);
                    }
                }

                std::string validation_error;
                status = ValidateForeignKeysChecked(
                        metadata.foreign_keys,
                        bson,
                        validation_error);
                if (!status.ok()) {
                    if (!validation_error.empty()) {
                        return DBResult::Err(validation_error);
                    }
                    return RocksFailure(
                            "InsertMany foreign-key validation",
                            status);
                }

                std::string doc_id = ExtractField(bson, "_id");
                if (doc_id.empty()) {
                    doc_id = GenerateDocId();
                }

                std::string old_document;
                status = mutation.ReadDocumentForUpdate(
                        doc_id,
                        old_document);
                if (status.ok()) {
                    return DBResult::Err(
                            "Duplicate document _id: " + doc_id);
                }

                if (!status.IsNotFound()) {
                    return RocksFailure(
                            "InsertMany document lookup",
                            status);
                }

                status = mutation.PutDocument(doc_id, bson);

                if (!status.ok()) {
                    return RocksFailure(
                            "InsertMany document/index staging",
                            status);
                }

                created_ids.push_back(std::move(doc_id));
            }

            status = mutation.AdjustCounter(
                    static_cast<std::int64_t>(created_ids.size()));
            if (!status.ok()) {
                return RocksFailure(
                        "InsertMany counter staging",
                        status);
            }

            status = mutation.Commit();
            if (!status.ok()) {
                return RocksFailure("InsertMany commit", status);
            }

            // ساخت JSON آرایه IDs
            std::string ids_json = "[";
            for (size_t i = 0; i < created_ids.size(); ++i) {
                if (i > 0) ids_json += ",";
                ids_json += "\"" + created_ids[i] + "\"";
            }
            ids_json += "]";
            return DBResult::Ok(ids_json);
        }

        BulkWriteResult DocEngine::InsertManyBulk(
                const std::string& collection_name,
                const std::vector<std::string>& bson_documents,
                const BulkWriteOptions& options) {
            BulkWriteResult result;
            if (options.mode == BulkWriteMode::Atomic) {
                const DBResult atomic = InsertMany(
                        collection_name,
                        bson_documents);
                result.success = atomic.success;
                result.processed = atomic.success
                        ? bson_documents.size() : 0;
                result.modified = result.processed;
                result.committed_chunks = atomic.success ? 1 : 0;
                result.last_error = atomic.error_msg;
                return result;
            }
            if (bson_documents.empty()) {
                result.last_error = "Document list is empty";
                return result;
            }
            if (options.max_operations_per_chunk == 0 ||
                options.max_bytes_per_chunk == 0) {
                result.last_error =
                        "Bulk chunk limits must be greater than zero";
                return result;
            }

            std::size_t offset = 0;
            while (offset < bson_documents.size()) {
                auto transaction = BeginTransaction();
                if (!transaction) {
                    result.last_error =
                            "InsertManyBulk: unable to start transaction";
                    return result;
                }

                std::size_t chunk_operations = 0;
                std::size_t chunk_bytes = 0;
                std::uint64_t chunk_modified = 0;
                while (offset < bson_documents.size() &&
                       chunk_operations < options.max_operations_per_chunk) {
                    const std::size_t document_bytes =
                            bson_documents[offset].size();
                    if (chunk_operations > 0 &&
                        chunk_bytes + document_bytes >
                                options.max_bytes_per_chunk) {
                        break;
                    }

                    const DBResult inserted = InsertOneTx(
                            *transaction,
                            collection_name,
                            bson_documents[offset]);
                    ++result.processed;
                    if (!inserted.success) {
                        const DBResult rollback =
                                RollbackTransaction(*transaction);
                        result.last_error = inserted.error_msg;
                        if (!rollback.success) {
                            result.last_error +=
                                    "; rollback failed: " +
                                    rollback.error_msg;
                        }
                        return result;
                    }

                    ++offset;
                    ++chunk_operations;
                    ++chunk_modified;
                    chunk_bytes += document_bytes;
                }

                const DBResult committed =
                        CommitTransaction(*transaction);
                if (!committed.success) {
                    result.last_error = committed.error_msg;
                    return result;
                }
                result.modified += chunk_modified;
                ++result.committed_chunks;
            }

            result.success = true;
            return result;
        }

// ══════════════════════════════════════════════════════════════
// §10  CRUD - Find
// ══════════════════════════════════════════════════════════════

        DBResult DocEngine::FindById(const std::string& collection_name,
                                     const std::string& doc_id) {
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");
            if (!CollectionExists(collection_name))
                return DBResult::Err("Collection '" + collection_name + "' does not exist");
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
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");
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

        DBResult DocEngine::UpdateById(
                const std::string& collection_name,
                const std::string& doc_id,
                const nexora::query::UpdateSpec& update_spec) {
            if (IsReservedCollectionName(collection_name)) {
                return DBResult::Err("reserved collection name");
            }

            MutationMetadata metadata;
            rocksdb::Status status = LoadMutationMetadata(
                    collection_name,
                    metadata);

            if (status.IsNotFound()) {
                return DBResult::Err(
                        "Collection '" + collection_name +
                        "' does not exist");
            }

            if (!status.ok()) {
                return RocksFailure("UpdateById metadata", status);
            }

            MutationBuilder mutation(
                    *this,
                    collection_name,
                    metadata.indexes);

            if (!mutation.IsValid()) {
                return DBResult::Err(
                        "UpdateById: unable to start transaction");
            }

            std::string old_document;
            status = mutation.ReadDocumentForUpdate(
                    doc_id,
                    old_document);

            if (status.IsNotFound()) {
                return DBResult::Err("Document not found: " + doc_id);
            }

            if (!status.ok()) {
                return RocksFailure("UpdateById document read", status);
            }

            const std::string new_document =
                    ApplyUpdate(old_document, update_spec);

            if (!metadata.schema.fields.empty()) {
                std::string validation_error;
                if (!ValidateDocument(
                        new_document,
                        metadata.schema,
                        validation_error)) {
                    return DBResult::Err(
                            "Schema validation failed: " +
                            validation_error);
                }
            }

            std::string validation_error;
            status = ValidateForeignKeysChecked(
                    metadata.foreign_keys,
                    new_document,
                    validation_error);

            if (!status.ok()) {
                if (!validation_error.empty()) {
                    return DBResult::Err(validation_error);
                }
                return RocksFailure(
                        "UpdateById foreign-key validation",
                        status);
            }

            status = mutation.ReplaceDocument(
                    doc_id,
                    old_document,
                    new_document);

            if (!status.ok()) {
                return RocksFailure(
                        "UpdateById document/index staging",
                        status);
            }

            status = mutation.Commit();
            if (!status.ok()) {
                return RocksFailure("UpdateById commit", status);
            }

            return DBResult::Ok("1");
        }

        DBResult DocEngine::UpdateMany(const std::string&               collection_name,
                                       const nexora::query::Condition&  condition,
                                       const nexora::query::UpdateSpec& update_spec) {
            if (IsReservedCollectionName(collection_name)) {
                return DBResult::Err("reserved collection name");
            }

            MutationMetadata metadata;
            rocksdb::Status status = LoadMutationMetadata(
                    collection_name,
                    metadata);

            if (status.IsNotFound()) {
                return DBResult::Err(
                        "Collection '" + collection_name +
                        "' does not exist");
            }
            if (!status.ok()) {
                return RocksFailure("UpdateMany metadata", status);
            }

            MutationBuilder mutation(
                    *this,
                    collection_name,
                    metadata.indexes);
            if (!mutation.IsValid()) {
                return DBResult::Err(
                        "UpdateMany: unable to start transaction");
            }

            const std::string data_prefix =
                    std::string(keys::kData) + collection_name + ":";
            std::vector<std::string> candidate_ids;

            {
                auto iterator = mutation.NewIterator(read_options_);
                for (iterator->Seek(data_prefix);
                     iterator->Valid() &&
                     iterator->key().starts_with(data_prefix);
                     iterator->Next()) {
                    const std::string document =
                            iterator->value().ToString();
                    if (MatchesCondition(document, condition)) {
                        candidate_ids.push_back(
                                iterator->key().ToString().substr(
                                        data_prefix.size()));
                    }
                }

                status = iterator->status();
                if (!status.ok()) {
                    return RocksFailure("UpdateMany scan", status);
                }
            }

            std::uint64_t updated = 0;
            for (const auto& doc_id : candidate_ids) {
                std::string old_document;
                status = mutation.ReadDocumentForUpdate(
                        doc_id,
                        old_document);

                if (status.IsNotFound()) {
                    continue;
                }
                if (!status.ok()) {
                    return RocksFailure(
                            "UpdateMany document read",
                            status);
                }
                if (!MatchesCondition(old_document, condition)) {
                    continue;
                }

                const std::string new_document =
                        ApplyUpdate(old_document, update_spec);

                if (!metadata.schema.fields.empty()) {
                    std::string validation_error;
                    if (!ValidateDocument(
                            new_document,
                            metadata.schema,
                            validation_error)) {
                        return DBResult::Err(
                                "Schema validation failed: " +
                                validation_error);
                    }
                }

                std::string validation_error;
                status = ValidateForeignKeysChecked(
                        metadata.foreign_keys,
                        new_document,
                        validation_error);
                if (!status.ok()) {
                    if (!validation_error.empty()) {
                        return DBResult::Err(validation_error);
                    }
                    return RocksFailure(
                            "UpdateMany foreign-key validation",
                            status);
                }

                status = mutation.ReplaceDocument(
                        doc_id,
                        old_document,
                        new_document);
                if (!status.ok()) {
                    return RocksFailure(
                            "UpdateMany document/index staging",
                            status);
                }
                ++updated;
            }

            status = mutation.Commit();
            if (!status.ok()) {
                return RocksFailure("UpdateMany commit", status);
            }

            return DBResult::Ok(std::to_string(updated));
        }

        BulkWriteResult DocEngine::UpdateManyBulk(
                const std::string& collection_name,
                const nexora::query::Condition& condition,
                const nexora::query::UpdateSpec& update_spec,
                const BulkWriteOptions& options) {
            BulkWriteResult result;
            if (options.mode == BulkWriteMode::Atomic) {
                const DBResult atomic = UpdateMany(
                        collection_name,
                        condition,
                        update_spec);
                result.success = atomic.success;
                result.last_error = atomic.error_msg;
                if (atomic.success) {
                    result.modified = std::stoull(atomic.data);
                    result.processed = result.modified;
                    result.committed_chunks = 1;
                }
                return result;
            }
            if (options.max_operations_per_chunk == 0 ||
                options.max_bytes_per_chunk == 0) {
                result.last_error =
                        "Bulk chunk limits must be greater than zero";
                return result;
            }
            if (!CollectionExists(collection_name)) {
                result.last_error =
                        "Collection '" + collection_name +
                        "' does not exist";
                return result;
            }

            const std::string prefix =
                    std::string(keys::kData) + collection_name + ":";
            std::vector<std::pair<std::string, std::size_t>> candidates;
            auto iterator = std::unique_ptr<rocksdb::Iterator>(
                    txn_db_->NewIterator(read_options_));
            for (iterator->Seek(prefix);
                 iterator->Valid() && iterator->key().starts_with(prefix);
                 iterator->Next()) {
                const std::string document = iterator->value().ToString();
                if (MatchesCondition(document, condition)) {
                    candidates.emplace_back(
                            iterator->key().ToString().substr(prefix.size()),
                            iterator->key().size() + document.size());
                }
            }
            if (!iterator->status().ok()) {
                result.last_error = RocksFailure(
                        "UpdateManyBulk scan",
                        iterator->status()).error_msg;
                return result;
            }

            std::size_t offset = 0;
            while (offset < candidates.size()) {
                auto transaction = BeginTransaction();
                if (!transaction) {
                    result.last_error =
                            "UpdateManyBulk: unable to start transaction";
                    return result;
                }
                std::size_t chunk_operations = 0;
                std::size_t chunk_bytes = 0;
                std::uint64_t chunk_modified = 0;
                while (offset < candidates.size() &&
                       chunk_operations < options.max_operations_per_chunk) {
                    const std::size_t operation_bytes = candidates[offset].second;
                    if (chunk_operations > 0 &&
                        chunk_bytes + operation_bytes >
                                options.max_bytes_per_chunk) {
                        break;
                    }
                    const DBResult updated = UpdateByIdTx(
                            *transaction,
                            collection_name,
                            candidates[offset].first,
                            update_spec);
                    ++result.processed;
                    if (!updated.success) {
                        const DBResult rollback =
                                RollbackTransaction(*transaction);
                        result.last_error = updated.error_msg;
                        if (!rollback.success) {
                            result.last_error +=
                                    "; rollback failed: " +
                                    rollback.error_msg;
                        }
                        return result;
                    }
                    if (updated.data == "1") ++chunk_modified;
                    ++offset;
                    ++chunk_operations;
                    chunk_bytes += operation_bytes;
                }
                const DBResult committed =
                        CommitTransaction(*transaction);
                if (!committed.success) {
                    result.last_error = committed.error_msg;
                    return result;
                }
                result.modified += chunk_modified;
                ++result.committed_chunks;
            }
            result.success = true;
            return result;
        }

// ══════════════════════════════════════════════════════════════
// §12  CRUD - Delete
// ══════════════════════════════════════════════════════════════

        DBResult DocEngine::DeleteById(
                const std::string& collection_name,
                const std::string& doc_id) {
            if (IsReservedCollectionName(collection_name)) {
                return DBResult::Err("reserved collection name");
            }

            MutationMetadata metadata;
            rocksdb::Status status = LoadMutationMetadata(
                    collection_name,
                    metadata);

            if (status.IsNotFound()) {
                return DBResult::Err(
                        "Collection '" + collection_name +
                        "' does not exist");
            }

            if (!status.ok()) {
                return RocksFailure("DeleteById metadata", status);
            }

            MutationBuilder mutation(
                    *this,
                    collection_name,
                    metadata.indexes);

            if (!mutation.IsValid()) {
                return DBResult::Err(
                        "DeleteById: unable to start transaction");
            }

            std::string old_document;
            status = mutation.ReadDocumentForUpdate(
                    doc_id,
                    old_document);

            if (status.IsNotFound()) {
                status = mutation.Rollback();
                if (!status.ok()) {
                    return RocksFailure(
                            "DeleteById rollback",
                            status);
                }
                return DBResult::Ok("0");
            }

            if (!status.ok()) {
                return RocksFailure("DeleteById document read", status);
            }

            status = mutation.DeleteDocument(
                    doc_id,
                    old_document);

            if (!status.ok()) {
                return RocksFailure(
                        "DeleteById document/index staging",
                        status);
            }

            status = mutation.AdjustCounter(-1);
            if (!status.ok()) {
                return RocksFailure(
                        "DeleteById counter staging",
                        status);
            }

            status = mutation.Commit();
            if (!status.ok()) {
                return RocksFailure("DeleteById commit", status);
            }

            return DBResult::Ok("1");
        }

        DBResult DocEngine::DeleteMany(const std::string&              collection_name,
                                       const nexora::query::Condition& condition) {
            if (IsReservedCollectionName(collection_name)) {
                return DBResult::Err("reserved collection name");
            }

            MutationMetadata metadata;
            rocksdb::Status status = LoadMutationMetadata(
                    collection_name,
                    metadata);

            if (status.IsNotFound()) {
                return DBResult::Err(
                        "Collection '" + collection_name +
                        "' does not exist");
            }
            if (!status.ok()) {
                return RocksFailure("DeleteMany metadata", status);
            }

            MutationBuilder mutation(
                    *this,
                    collection_name,
                    metadata.indexes);
            if (!mutation.IsValid()) {
                return DBResult::Err(
                        "DeleteMany: unable to start transaction");
            }

            const std::string data_prefix =
                    std::string(keys::kData) + collection_name + ":";
            std::vector<std::string> candidate_ids;

            {
                auto iterator = mutation.NewIterator(read_options_);
                for (iterator->Seek(data_prefix);
                     iterator->Valid() &&
                     iterator->key().starts_with(data_prefix);
                     iterator->Next()) {
                    const std::string document =
                            iterator->value().ToString();
                    if (MatchesCondition(document, condition)) {
                        candidate_ids.push_back(
                                iterator->key().ToString().substr(
                                        data_prefix.size()));
                    }
                }

                status = iterator->status();
                if (!status.ok()) {
                    return RocksFailure("DeleteMany scan", status);
                }
            }

            std::uint64_t deleted = 0;
            for (const auto& doc_id : candidate_ids) {
                std::string old_document;
                status = mutation.ReadDocumentForUpdate(
                        doc_id,
                        old_document);

                if (status.IsNotFound()) {
                    continue;
                }
                if (!status.ok()) {
                    return RocksFailure(
                            "DeleteMany document read",
                            status);
                }
                if (!MatchesCondition(old_document, condition)) {
                    continue;
                }

                status = mutation.DeleteDocument(
                        doc_id,
                        old_document);
                if (!status.ok()) {
                    return RocksFailure(
                            "DeleteMany document/index staging",
                            status);
                }
                ++deleted;
            }

            if (deleted > 0) {
                status = mutation.AdjustCounter(
                        -static_cast<std::int64_t>(deleted));
                if (!status.ok()) {
                    return RocksFailure(
                            "DeleteMany counter staging",
                            status);
                }
            }

            status = mutation.Commit();
            if (!status.ok()) {
                return RocksFailure("DeleteMany commit", status);
            }

            return DBResult::Ok(std::to_string(deleted));
        }

        BulkWriteResult DocEngine::DeleteManyBulk(
                const std::string& collection_name,
                const nexora::query::Condition& condition,
                const BulkWriteOptions& options) {
            BulkWriteResult result;
            if (options.mode == BulkWriteMode::Atomic) {
                const DBResult atomic = DeleteMany(collection_name, condition);
                result.success = atomic.success;
                result.last_error = atomic.error_msg;
                if (atomic.success) {
                    result.modified = std::stoull(atomic.data);
                    result.processed = result.modified;
                    result.committed_chunks = 1;
                }
                return result;
            }
            if (options.max_operations_per_chunk == 0 ||
                options.max_bytes_per_chunk == 0) {
                result.last_error =
                        "Bulk chunk limits must be greater than zero";
                return result;
            }
            if (!CollectionExists(collection_name)) {
                result.last_error =
                        "Collection '" + collection_name +
                        "' does not exist";
                return result;
            }

            const std::string prefix =
                    std::string(keys::kData) + collection_name + ":";
            std::vector<std::pair<std::string, std::size_t>> candidates;
            auto iterator = std::unique_ptr<rocksdb::Iterator>(
                    txn_db_->NewIterator(read_options_));
            for (iterator->Seek(prefix);
                 iterator->Valid() && iterator->key().starts_with(prefix);
                 iterator->Next()) {
                const std::string document = iterator->value().ToString();
                if (MatchesCondition(document, condition)) {
                    candidates.emplace_back(
                            iterator->key().ToString().substr(prefix.size()),
                            iterator->key().size() + document.size());
                }
            }
            if (!iterator->status().ok()) {
                result.last_error = RocksFailure(
                        "DeleteManyBulk scan",
                        iterator->status()).error_msg;
                return result;
            }

            std::size_t offset = 0;
            while (offset < candidates.size()) {
                auto transaction = BeginTransaction();
                if (!transaction) {
                    result.last_error =
                            "DeleteManyBulk: unable to start transaction";
                    return result;
                }
                std::size_t chunk_operations = 0;
                std::size_t chunk_bytes = 0;
                std::uint64_t chunk_modified = 0;
                while (offset < candidates.size() &&
                       chunk_operations < options.max_operations_per_chunk) {
                    const std::size_t operation_bytes = candidates[offset].second;
                    if (chunk_operations > 0 &&
                        chunk_bytes + operation_bytes >
                                options.max_bytes_per_chunk) {
                        break;
                    }
                    const DBResult deleted = DeleteByIdTx(
                            *transaction,
                            collection_name,
                            candidates[offset].first);
                    ++result.processed;
                    if (!deleted.success) {
                        const DBResult rollback =
                                RollbackTransaction(*transaction);
                        result.last_error = deleted.error_msg;
                        if (!rollback.success) {
                            result.last_error +=
                                    "; rollback failed: " +
                                    rollback.error_msg;
                        }
                        return result;
                    }
                    if (deleted.data == "1") ++chunk_modified;
                    ++offset;
                    ++chunk_operations;
                    chunk_bytes += operation_bytes;
                }
                const DBResult committed =
                        CommitTransaction(*transaction);
                if (!committed.success) {
                    result.last_error = committed.error_msg;
                    return result;
                }
                result.modified += chunk_modified;
                ++result.committed_chunks;
            }
            result.success = true;
            return result;
        }

// ══════════════════════════════════════════════════════════════
// §13  Internal API برای GraphEngine
// ══════════════════════════════════════════════════════════════

        void DocEngine::IterateCollection(const std::string&      collection_name,
                                          const DocumentCallback& callback,
                                          uint32_t                batch_size) const {
            if (IsReservedCollectionName(collection_name)) {
                NX_LOG("IterateCollection: reserved collection name");
                return;
            }
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
            if (IsReservedCollectionName(collection_name)) return -1;
            std::string seq_key = std::string(keys::kSeq) + collection_name;
            std::string val;
            rocksdb::Status s = txn_db_->Get(read_options_, seq_key, &val);
            if (!s.ok() || val.empty()) return -1;
            try { return std::stoll(val); } catch (...) { return -1; }
        }

        DBResult DocEngine::ReconcileCollectionCounter(
                const std::string& collection_name) {
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");

            MutationMetadata metadata;
            rocksdb::Status status = LoadMutationMetadata(
                    collection_name,
                    metadata);
            if (status.IsNotFound())
                return DBResult::Err(
                        "Collection '" + collection_name +
                        "' does not exist");
            if (!status.ok())
                return RocksFailure(
                        "ReconcileCollectionCounter metadata",
                        status);

            std::unique_ptr<rocksdb::Transaction> transaction(
                    txn_db_->BeginTransaction(write_options_));
            if (!transaction)
                return DBResult::Err(
                        "ReconcileCollectionCounter: unable to start transaction");

            const std::string counter_key =
                    std::string(keys::kSeq) + collection_name;
            std::string old_counter;
            status = transaction->GetForUpdate(
                    read_options_,
                    counter_key,
                    &old_counter);
            if (!status.ok() && !status.IsNotFound())
                return RocksFailure(
                        "ReconcileCollectionCounter counter lock",
                        status);

            const std::string data_prefix =
                    std::string(keys::kData) + collection_name + ":";
            const rocksdb::Slice prefix_slice(data_prefix);
            std::uint64_t document_count = 0;

            auto iterator = std::unique_ptr<rocksdb::Iterator>(
                    transaction->GetIterator(read_options_));
            for (iterator->Seek(data_prefix);
                 iterator->Valid() &&
                 iterator->key().starts_with(prefix_slice);
                 iterator->Next()) {
                if (document_count ==
                    static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max()))
                    return DBResult::Err(
                            "Collection counter exceeds int64 range");
                ++document_count;
            }

            status = iterator->status();
            if (!status.ok())
                return RocksFailure(
                        "ReconcileCollectionCounter data scan",
                        status);

            status = transaction->Put(
                    counter_key,
                    std::to_string(document_count));
            if (!status.ok())
                return RocksFailure(
                        "ReconcileCollectionCounter counter write",
                        status);

            status = transaction->Commit();
            if (!status.ok())
                return RocksFailure(
                        "ReconcileCollectionCounter commit",
                        status);

            return DBResult::Ok(std::to_string(document_count));
        }

        std::vector<std::pair<std::string, std::string>>
        DocEngine::GetDocumentRange(const std::string& collection_name,
                                    const std::string& id_prefix,
                                    uint32_t           max_count) const {
            std::vector<std::pair<std::string, std::string>> results;
            if (IsReservedCollectionName(collection_name)) return results;
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

            if (IsReservedCollectionName(from_collection) ||
                IsReservedCollectionName(to_collection)) {
                result.error_msg = "reserved collection name";
                return result;
            }

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
            rocksdb::Transaction* tx = txn_db_->BeginTransaction(
                    wo,
                    transaction_options_);
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
            if (!s.ok()) {
                tx_handle.Reset();
                return RocksFailure("CommitTransaction", s);
            }
            tx_handle.Reset();
            return DBResult::Ok("Transaction committed");
        }

        DBResult DocEngine::RollbackTransaction(TxHandle& tx_handle) {
            if (!tx_handle.IsValid())
                return DBResult::Err("Invalid transaction handle");
            rocksdb::Status s = tx_handle.Get()->Rollback();
            if (!s.ok()) {
                tx_handle.Reset();
                return RocksFailure("RollbackTransaction", s);
            }
            tx_handle.Reset();
            return DBResult::Ok("Transaction rolled back");
        }

        DBResult DocEngine::InsertOneTx(TxHandle&          tx_handle,
                                        const std::string& collection_name,
                                        const std::string& bson_document) {
            if (!tx_handle.IsValid()) return DBResult::Err("Invalid transaction");
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");

            MutationMetadata metadata;
            rocksdb::Status status = LoadMutationMetadata(
                    collection_name,
                    metadata);
            if (status.IsNotFound())
                return DBResult::Err(
                        "Collection '" + collection_name +
                        "' does not exist");
            if (!status.ok())
                return RocksFailure("InsertOneTx metadata", status);

            if (!metadata.schema.fields.empty()) {
                std::string validation_error;
                if (!ValidateDocument(
                        bson_document,
                        metadata.schema,
                        validation_error))
                    return DBResult::Err(
                            "Schema validation failed: " +
                            validation_error);
            }

            std::string validation_error;
            status = ValidateForeignKeysChecked(
                    metadata.foreign_keys,
                    bson_document,
                    validation_error,
                    tx_handle.Get());
            if (!status.ok()) {
                if (!validation_error.empty())
                    return DBResult::Err(validation_error);
                return RocksFailure(
                        "InsertOneTx foreign-key validation",
                        status);
            }

            std::string doc_id = ExtractField(bson_document, "_id");
            if (doc_id.empty()) doc_id = GenerateDocId();

            MutationBuilder mutation(
                    *this,
                    *tx_handle.Get(),
                    collection_name,
                    metadata.indexes);

            std::string old_document;
            status = mutation.ReadDocumentForUpdate(
                    doc_id,
                    old_document);
            if (status.ok())
                return DBResult::Err(
                        "Duplicate document _id: " + doc_id);

            if (!status.IsNotFound())
                return RocksFailure("InsertOneTx document lookup", status);

            status = mutation.PutDocument(doc_id, bson_document);
            if (!status.ok())
                return RocksFailure(
                        "InsertOneTx document/index staging",
                        status);

            status = mutation.AdjustCounter(1);
            if (!status.ok())
                return RocksFailure(
                        "InsertOneTx counter staging",
                        status);

            status = mutation.Commit();
            if (!status.ok())
                return RocksFailure("InsertOneTx savepoint", status);

            return DBResult::Ok(doc_id);
        }

        DBResult DocEngine::UpdateByIdTx(TxHandle&                        tx_handle,
                                         const std::string&               collection_name,
                                         const std::string&               doc_id,
                                         const nexora::query::UpdateSpec& update_spec) {
            if (!tx_handle.IsValid()) return DBResult::Err("Invalid transaction");
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");

            MutationMetadata metadata;
            rocksdb::Status status = LoadMutationMetadata(
                    collection_name,
                    metadata);
            if (status.IsNotFound())
                return DBResult::Err(
                        "Collection '" + collection_name +
                        "' does not exist");
            if (!status.ok())
                return RocksFailure("UpdateByIdTx metadata", status);

            MutationBuilder mutation(
                    *this,
                    *tx_handle.Get(),
                    collection_name,
                    metadata.indexes);

            std::string old_document;
            status = mutation.ReadDocumentForUpdate(
                    doc_id,
                    old_document);
            if (status.IsNotFound())
                return DBResult::Err("Document not found: " + doc_id);
            if (!status.ok())
                return RocksFailure("UpdateByIdTx document read", status);

            const std::string new_document =
                    ApplyUpdate(old_document, update_spec);

            if (!metadata.schema.fields.empty()) {
                std::string validation_error;
                if (!ValidateDocument(
                        new_document,
                        metadata.schema,
                        validation_error))
                    return DBResult::Err(
                            "Schema validation failed: " +
                            validation_error);
            }

            std::string validation_error;
            status = ValidateForeignKeysChecked(
                    metadata.foreign_keys,
                    new_document,
                    validation_error,
                    tx_handle.Get());
            if (!status.ok()) {
                if (!validation_error.empty())
                    return DBResult::Err(validation_error);
                return RocksFailure(
                        "UpdateByIdTx foreign-key validation",
                        status);
            }

            status = mutation.ReplaceDocument(
                    doc_id,
                    old_document,
                    new_document);
            if (!status.ok())
                return RocksFailure(
                        "UpdateByIdTx document/index staging",
                        status);

            status = mutation.Commit();
            if (!status.ok())
                return RocksFailure("UpdateByIdTx savepoint", status);

            return DBResult::Ok("1");
        }

        DBResult DocEngine::DeleteByIdTx(TxHandle&          tx_handle,
                                         const std::string& collection_name,
                                         const std::string& doc_id) {
            if (!tx_handle.IsValid()) return DBResult::Err("Invalid transaction");
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");

            MutationMetadata metadata;
            rocksdb::Status status = LoadMutationMetadata(
                    collection_name,
                    metadata);
            if (status.IsNotFound())
                return DBResult::Err(
                        "Collection '" + collection_name +
                        "' does not exist");
            if (!status.ok())
                return RocksFailure("DeleteByIdTx metadata", status);

            MutationBuilder mutation(
                    *this,
                    *tx_handle.Get(),
                    collection_name,
                    metadata.indexes);

            std::string old_document;
            status = mutation.ReadDocumentForUpdate(
                    doc_id,
                    old_document);
            if (status.IsNotFound()) {
                status = mutation.Rollback();
                if (!status.ok())
                    return RocksFailure("DeleteByIdTx savepoint rollback", status);
                return DBResult::Ok("0");
            }
            if (!status.ok())
                return RocksFailure("DeleteByIdTx document read", status);

            status = mutation.DeleteDocument(
                    doc_id,
                    old_document);
            if (!status.ok())
                return RocksFailure(
                        "DeleteByIdTx document/index staging",
                        status);

            status = mutation.AdjustCounter(-1);
            if (!status.ok())
                return RocksFailure(
                        "DeleteByIdTx counter staging",
                        status);

            status = mutation.Commit();
            if (!status.ok())
                return RocksFailure("DeleteByIdTx savepoint", status);

            return DBResult::Ok("1");
        }

        DBResult DocEngine::FindByIdTx(TxHandle&          tx_handle,
                                       const std::string& collection_name,
                                       const std::string& doc_id) {
            if (!tx_handle.IsValid()) return DBResult::Err("Invalid transaction");
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");

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
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");
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
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");
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
// §17  Internal database users
// ══════════════════════════════════════════════════════════════

        DBResult DocEngine::CreateInternalUser(const std::string& user_json) {
            if (!EnsureInternalCollections())
                return DBResult::Err("failed to initialize internal collections");

            std::string username;
            std::string err;
            if (!ValidateInternalUserDocument(user_json, username, err))
                return DBResult::Err(err);

            const std::string role = ExtractField(user_json, "role");
            if (username == "root" && role != "admin")
                return DBResult::Err("root user must be admin");

            const std::vector<IndexDefinition> indexes;
            MutationBuilder mutation(*this, keys::kInternalUsers, indexes);
            if (!mutation.IsValid())
                return DBResult::Err(
                        "CreateInternalUser: unable to start transaction");

            std::string existing;
            rocksdb::Status s = mutation.ReadDocumentForUpdate(
                    username,
                    existing);
            if (s.ok()) return DBResult::Err("internal user already exists");
            if (!s.IsNotFound())
                return RocksFailure("CreateInternalUser read", s);

            s = mutation.PutDocument(username, user_json);
            if (!s.ok()) return RocksFailure("CreateInternalUser write", s);
            s = mutation.AdjustCounter(1);
            if (!s.ok()) return RocksFailure("CreateInternalUser counter", s);
            s = mutation.Commit();
            if (!s.ok()) return RocksFailure("CreateInternalUser commit", s);

            return DBResult::Ok(username);
        }

        DBResult DocEngine::GetInternalUser(const std::string& username) {
            if (username.empty()) return DBResult::Err("username is required");

            std::string user_json;
            rocksdb::Status s = txn_db_->Get(read_options_,
                                             MakeDocKey(keys::kInternalUsers, username),
                                             &user_json);
            if (s.IsNotFound()) return DBResult::Err("internal user not found");
            ROCKS_CHECK(s, "GetInternalUser");
            return DBResult::Ok(user_json);
        }

        DBResult DocEngine::UpdateInternalUser(const std::string& username,
                                               const std::string& user_json) {
            if (username.empty()) return DBResult::Err("username is required");

            std::string parsed_username;
            std::string err;
            if (!ValidateInternalUserDocument(user_json, parsed_username, err))
                return DBResult::Err(err);
            if (parsed_username != username)
                return DBResult::Err("username cannot be changed");

            const std::string role = ExtractField(user_json, "role");
            if (username == "root" && role != "admin")
                return DBResult::Err("root user must be admin");

            const std::vector<IndexDefinition> indexes;
            MutationBuilder mutation(*this, keys::kInternalUsers, indexes);
            if (!mutation.IsValid())
                return DBResult::Err(
                        "UpdateInternalUser: unable to start transaction");
            std::string existing;
            rocksdb::Status s = mutation.ReadDocumentForUpdate(
                    username,
                    existing);
            if (s.IsNotFound()) return DBResult::Err("internal user not found");
            if (!s.ok()) return RocksFailure("UpdateInternalUser read", s);

            s = mutation.ReplaceDocument(username, existing, user_json);
            if (!s.ok()) return RocksFailure("UpdateInternalUser write", s);
            s = mutation.Commit();
            if (!s.ok()) return RocksFailure("UpdateInternalUser commit", s);
            return DBResult::Ok("1");
        }

        DBResult DocEngine::DeleteInternalUser(const std::string& username) {
            if (username.empty()) return DBResult::Err("username is required");
            if (username == "root") return DBResult::Err("cannot delete root user");

            const std::vector<IndexDefinition> indexes;
            MutationBuilder mutation(*this, keys::kInternalUsers, indexes);
            if (!mutation.IsValid())
                return DBResult::Err(
                        "DeleteInternalUser: unable to start transaction");
            std::string existing;
            rocksdb::Status s = mutation.ReadDocumentForUpdate(
                    username,
                    existing);
            if (s.IsNotFound()) return DBResult::Err("internal user not found");
            if (!s.ok()) return RocksFailure("DeleteInternalUser read", s);

            nexora::query::UpdateSpec spec;
            spec.Set("status", "deleted");
            spec.TouchDate("updated_at");
            const std::string deleted_json = ApplyUpdate(existing, spec);

            s = mutation.ReplaceDocument(username, existing, deleted_json);
            if (!s.ok()) return RocksFailure("DeleteInternalUser write", s);
            s = mutation.Commit();
            if (!s.ok()) return RocksFailure("DeleteInternalUser commit", s);
            return DBResult::Ok("1");
        }

// ══════════════════════════════════════════════════════════════
// §18  Internal application tokens
// ══════════════════════════════════════════════════════════════

        DBResult DocEngine::CreateInternalAppToken(const std::string& token_id,
                                                   const std::string& token_json) {
            if (token_id.empty()) return DBResult::Err("token id is required");
            if (token_json.empty()) return DBResult::Err("token document is required");
            if (!EnsureInternalCollections())
                return DBResult::Err("failed to initialize internal collections");

            const std::vector<IndexDefinition> indexes;
            MutationBuilder mutation(
                    *this,
                    keys::kInternalAppTokens,
                    indexes);
            if (!mutation.IsValid())
                return DBResult::Err(
                        "CreateInternalAppToken: unable to start transaction");
            std::string existing;
            rocksdb::Status s = mutation.ReadDocumentForUpdate(
                    token_id,
                    existing);
            if (s.ok()) return DBResult::Err("application token already exists");
            if (!s.IsNotFound())
                return RocksFailure("CreateInternalAppToken read", s);

            s = mutation.PutDocument(token_id, token_json);
            if (!s.ok()) return RocksFailure("CreateInternalAppToken write", s);
            s = mutation.AdjustCounter(1);
            if (!s.ok()) return RocksFailure("CreateInternalAppToken counter", s);
            s = mutation.Commit();
            if (!s.ok()) return RocksFailure("CreateInternalAppToken commit", s);
            return DBResult::Ok(token_id);
        }

        DBResult DocEngine::ListInternalAppTokens() const {
            const std::string prefix = std::string(keys::kData) +
                                       keys::kInternalAppTokens + ":";
            auto iterator = std::unique_ptr<rocksdb::Iterator>(
                    txn_db_->NewIterator(read_options_));
            std::string json = "[";
            bool first = true;
            for (iterator->Seek(prefix);
                 iterator->Valid() && iterator->key().starts_with(prefix);
                 iterator->Next()) {
                if (!first) json += ',';
                first = false;
                json += iterator->value().ToString();
            }
            if (!iterator->status().ok())
                return DBResult::Err("[RocksDB] ListInternalAppTokens: " +
                                     iterator->status().ToString());
            json += ']';
            return DBResult::Ok(json);
        }

        DBResult DocEngine::DeleteInternalAppToken(const std::string& token_id) {
            if (token_id.empty()) return DBResult::Err("token id is required");
            const std::vector<IndexDefinition> indexes;
            MutationBuilder mutation(
                    *this,
                    keys::kInternalAppTokens,
                    indexes);
            if (!mutation.IsValid())
                return DBResult::Err(
                        "DeleteInternalAppToken: unable to start transaction");
            std::string existing;
            rocksdb::Status s = mutation.ReadDocumentForUpdate(
                    token_id,
                    existing);
            if (s.IsNotFound()) return DBResult::Err("application token not found");
            if (!s.ok()) return RocksFailure("DeleteInternalAppToken read", s);
            s = mutation.DeleteDocument(token_id, existing);
            if (!s.ok()) return RocksFailure("DeleteInternalAppToken write", s);
            s = mutation.AdjustCounter(-1);
            if (!s.ok()) return RocksFailure("DeleteInternalAppToken counter", s);
            s = mutation.Commit();
            if (!s.ok()) return RocksFailure("DeleteInternalAppToken commit", s);
            return DBResult::Ok("1");
        }

        bool DocEngine::IsInternalAppTokenActive(const std::string& token_id) const {
            if (token_id.empty()) return false;
            std::string value;
            return txn_db_->Get(read_options_,
                                MakeDocKey(keys::kInternalAppTokens, token_id),
                                &value).ok();
        }

// ══════════════════════════════════════════════════════════════
// §19  دسترسی به پیکربندی
// ══════════════════════════════════════════════════════════════

        std::optional<SchemaDefinition>
        DocEngine::GetSchema(const std::string& collection_name) const {
            if (IsReservedCollectionName(collection_name)) return std::nullopt;
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
            if (IsReservedCollectionName(collection_name)) return result;
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
            if (IsReservedCollectionName(collection_name)) return result;
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
            for (it->Seek(prefix); it->Valid() && it->key().starts_with(prefix); it->Next()) {
                std::string collection = it->key().ToString().substr(prefix.size());
                if (!IsReservedCollectionName(collection)) result.push_back(std::move(collection));
            }
            return result;
        }

        uint64_t DocEngine::GetRamUsageBytes() const {
#if defined(__linux__)
            std::ifstream statm("/proc/self/statm");
            uint64_t total_pages = 0;
            uint64_t resident_pages = 0;
            if (!(statm >> total_pages >> resident_pages)) return 0;

            long page_size = sysconf(_SC_PAGESIZE);
            if (page_size <= 0) return 0;
            return resident_pages * static_cast<uint64_t>(page_size);
#else
            return 0;
#endif
        }

        uint64_t DocEngine::GetDiskUsageBytes() const {
            namespace fs = std::filesystem;

            std::error_code ec;
            if (!fs::exists(db_path_, ec)) return 0;

            uint64_t total = 0;
            fs::recursive_directory_iterator it(
                    db_path_, fs::directory_options::skip_permission_denied, ec);
            fs::recursive_directory_iterator end;

            for (; it != end; it.increment(ec)) {
                if (ec) {
                    ec.clear();
                    continue;
                }
                if (!it->is_regular_file(ec)) continue;

                auto size = it->file_size(ec);
                if (!ec) total += static_cast<uint64_t>(size);
                ec.clear();
            }
            return total;
        }

    } // namespace core
} // namespace nexora
