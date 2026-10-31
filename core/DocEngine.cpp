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
#include "DocumentCodec.h"
#include "IndexCodec.h"
#include "query/CompiledQuery.h"
#include "query/CompiledUpdate.h"
#include "query/DocumentView.h"
#include <nlohmann/json.hpp>

// RocksDB
#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/status.h>
#include <rocksdb/utilities/transaction.h>
#include <rocksdb/utilities/transaction_db.h>
#include <rocksdb/write_batch.h>

// Standard Library
#include <algorithm>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

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
            constexpr char kDocumentFormat[] = "meta:format:documents";
            constexpr char kDocumentMigrationCursor[] =
                    "meta:migration:documents:v1";
        } // namespace keys



// ══════════════════════════════════════════════════════════════
        namespace {
            class ReadEpoch {
                rocksdb::DB* db_;
                const rocksdb::Snapshot* snapshot_;
                bool owned_;
            public:
                explicit ReadEpoch(rocksdb::DB* db,const rocksdb::Snapshot* borrowed=nullptr)
                    :db_(db),snapshot_(borrowed ? borrowed : db->GetSnapshot()),owned_(!borrowed) {
                    if (!snapshot_) throw std::runtime_error("unable to acquire RocksDB read snapshot");
                }
                ~ReadEpoch() { if (owned_) db_->ReleaseSnapshot(snapshot_); }
                const rocksdb::Snapshot* get() const { return snapshot_; }
            };
            rocksdb::Status MatchDocument(
                    std::string_view encoded_or_json,
                    const nexora::query::CompiledQuery& query,
                    bool& matched) {
                CheckExecution();
                std::string owned_bson;
                std::string_view payload;
                switch (DocumentCodec::Detect(encoded_or_json)) {
                    case DocumentCodec::Format::BsonV1:
                        payload = encoded_or_json.substr(
                                DocumentCodec::kEnvelopeSize);
                        break;
                    case DocumentCodec::Format::LegacyJson: {
                        auto encoded = DocumentCodec::EncodeJson(encoded_or_json);
                        if (!encoded.success) {
                            return rocksdb::Status::InvalidArgument(encoded.error);
                        }
                        owned_bson = std::move(encoded.value);
                        payload = std::string_view(owned_bson).substr(
                                DocumentCodec::kEnvelopeSize);
                        break;
                    }
                    case DocumentCodec::Format::UnknownEnvelope:
                        return rocksdb::Status::Corruption(
                                "unsupported document envelope");
                }
                const nexora::query::DocumentView view(payload);
                if (!view.valid()) {
                    return rocksdb::Status::Corruption("invalid BSON document");
                }
                try {
                    matched = query.Match(view);
                } catch (const std::exception& exception) {
                    return rocksdb::Status::Aborted("Query evaluation failed", exception.what());
                }
                return rocksdb::Status::OK();
            }

            std::string ScalarToString(
                    const nexora::query::DocumentValueView& value) {
                switch (value.type) {
                    case nexora::query::DocumentValueType::String:
                        return std::string(value.string_value);
                    case nexora::query::DocumentValueType::Int64: {
                        char buffer[32];
                        const auto result = std::to_chars(
                                std::begin(buffer), std::end(buffer),
                                value.int_value);
                        return result.ec == std::errc{}
                                ? std::string(buffer, result.ptr) : std::string{};
                    }
                    case nexora::query::DocumentValueType::Float64: {
                        char buffer[64];
                        const auto result = std::to_chars(
                                std::begin(buffer), std::end(buffer),
                                value.double_value);
                        return result.ec == std::errc{}
                                ? std::string(buffer, result.ptr) : std::string{};
                    }
                    case nexora::query::DocumentValueType::Bool:
                        return value.bool_value ? "true" : "false";
                    default:
                        return {};
                }
            }

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
                        status.ToString(),status.IsCorruption()?"corruption":status.IsIOError()?"io_error":
                        status.IsDeadlock()?"transaction_deadlock":status.IsTimedOut()?"transaction_timeout":
                        status.IsBusy()?"transaction_conflict":"operation_failed");
            }

            std::string PrefixUpperBound(std::string prefix) {
                for (std::size_t i = prefix.size(); i > 0; --i) {
                    auto& byte = reinterpret_cast<unsigned char&>(prefix[i - 1]);
                    if (byte != 0xff) {
                        ++byte;
                        prefix.resize(i);
                        return prefix;
                    }
                }
                return {};
            }

            class PrefixIterator {
                std::string upper_;
                rocksdb::Slice bound_;
                rocksdb::ReadOptions options_;
                std::unique_ptr<rocksdb::Iterator> iterator_;
            public:
                PrefixIterator(rocksdb::DB* db,const std::string& prefix,const rocksdb::ReadOptions& base)
                    :upper_(PrefixUpperBound(prefix)),bound_(upper_),options_(MakeScanReadOptions(base)) {
                    if (!upper_.empty()) options_.iterate_upper_bound=&bound_;
                    iterator_.reset(db->NewIterator(options_));
                }
                PrefixIterator(const PrefixIterator&)=delete;
                rocksdb::Iterator* operator->() const { return iterator_.get(); }
            };

            std::string EncodeCursorToken(const std::string& key) {
                static constexpr char hex[] = "0123456789abcdef";
                std::string token = "d1.";
                token.reserve(3 + key.size() * 2);
                for (const unsigned char byte : key) {
                    token.push_back(hex[byte >> 4]);
                    token.push_back(hex[byte & 0x0f]);
                }
                return token;
            }

            std::optional<std::string> DecodeCursorToken(
                    const std::string& token) {
                if (!token.starts_with("d1.") || (token.size() - 3) % 2 != 0)
                    return std::nullopt;
                auto nibble = [](char value) -> int {
                    if (value >= '0' && value <= '9') return value - '0';
                    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
                    return -1;
                };
                std::string key;
                key.reserve((token.size() - 3) / 2);
                for (std::size_t i = 3; i < token.size(); i += 2) {
                    const int high = nibble(token[i]);
                    const int low = nibble(token[i + 1]);
                    if (high < 0 || low < 0) return std::nullopt;
                    key.push_back(static_cast<char>((high << 4) | low));
                }
                return key;
            }

            struct SelectedIndexPlan {
                const IndexDefinition* index = nullptr;
                std::vector<const nexora::query::Condition*> equalities;
                const nexora::query::Condition* range = nullptr;
                std::size_t score = 0;
            };

            bool CollectAndLeaves(
                    const nexora::query::Condition& condition,
                    std::vector<const nexora::query::Condition*>& leaves) {
                if (condition.IsLeaf()) {
                    leaves.push_back(&condition);
                    return true;
                }
                if (condition.IsEmpty()) return true;
                if (condition.logic != nexora::query::LogicOp::AND) return false;
                for (const auto& child : condition.sub_conditions)
                    if (!CollectAndLeaves(child, leaves)) return false;
                return true;
            }

            SelectedIndexPlan SelectIndexPlan(
                    const std::vector<IndexDefinition>& indexes,
                    const nexora::query::Condition& condition) {
                std::vector<const nexora::query::Condition*> leaves;
                if (!CollectAndLeaves(condition, leaves)) return {};
                SelectedIndexPlan best;
                for (const auto& index : indexes) {
                    if (index.state != IndexState::Ready ||
                        index.format_version != indexv2::kFormatVersion)
                        continue;
                    SelectedIndexPlan candidate;
                    candidate.index = &index;
                    for (const auto& field : index.fields) {
                        const auto equality = std::find_if(
                                leaves.begin(), leaves.end(),
                                [&](const auto* leaf) {
                                    return leaf->field == field &&
                                           leaf->op == nexora::query::Op::EQ;
                                });
                        if (equality != leaves.end()) {
                            // v3 canonical numeric equality; null/containers
                            // still have no sparse entries.
                            if ((*equality)->value_type!=query::ValueType::String &&
                                (*equality)->value_type!=query::ValueType::Bool &&
                                (*equality)->value_type!=query::ValueType::Int64 &&
                                (*equality)->value_type!=query::ValueType::Float64) break;
                            candidate.equalities.push_back(*equality);
                            candidate.score += 10;
                            continue;
                        }
                        const auto range = std::find_if(
                                leaves.begin(), leaves.end(),
                                [&](const auto* leaf) {
                                    return leaf->field == field &&
                                           (leaf->op == nexora::query::Op::GT ||
                                            leaf->op == nexora::query::Op::GTE ||
                                            leaf->op == nexora::query::Op::LT ||
                                            leaf->op == nexora::query::Op::LTE);
                                });
                        if (range != leaves.end() && (*range)->value_type==query::ValueType::String) {
                            candidate.range = *range;
                            candidate.score += 5;
                        }
                        break;
                    }
                    // A sparse compound entry is absent if ANY component is
                    // unindexable. Prefix-only scans cannot prove coverage.
                    const auto covered=candidate.equalities.size()+(candidate.range ? 1U : 0U);
                    if (covered==index.fields.size() && covered>0 &&
                        candidate.score > best.score)
                        best = candidate;
                }
                return best;
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
            auto encoded = DocumentCodec::EncodeJson(bson);
            if (!encoded.success) return {};
            const nexora::query::CompiledFieldPath path(field_name);
            if (!path.valid()) return {};
            const nexora::query::DocumentView view(
                    std::string_view(encoded.value).substr(
                            DocumentCodec::kEnvelopeSize));
            return view.valid() ? ScalarToString(view.Get(path)) : std::string{};
        }

        rocksdb::Status DocEngine::ApplyUpdate(
                const std::string& bson,
                const nexora::query::UpdateSpec& spec,
                std::string& updated_document) {
            for(const auto& operation:spec.operations) {
                const auto identity=[](const std::string& field){return field=="_id" || field.starts_with("_id.");};
                if(identity(operation.field) ||
                   (operation.op==query::UpdateOp::Rename && identity(operation.value)))
                    return rocksdb::Status::InvalidArgument("document _id is immutable");
            }
            const nexora::query::CompiledUpdate compiled(spec);
            if (!compiled.valid()) {
                return rocksdb::Status::InvalidArgument(compiled.error());
            }
            auto result = compiled.ApplyJson(bson);
            if (!result.success) {
                return rocksdb::Status::InvalidArgument(result.error);
            }
            updated_document = std::move(result.document);
            return rocksdb::Status::OK();
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
            out.append("NXI3", 4);
            serial::WriteString(out, idx.index_id);
            serial::WriteString(out, idx.index_name);
            serial::WriteByte(out, static_cast<uint8_t>(idx.type));
            uint32_t fc = static_cast<uint32_t>(idx.fields.size());
            out.append(reinterpret_cast<const char*>(&fc), 4);
            for (const auto& f : idx.fields) serial::WriteString(out, f);
            serial::WriteByte(out, static_cast<uint8_t>(idx.state));
            serial::WriteString(out, idx.build_cursor);
            serial::WriteString(out, idx.last_error);
            return out;
        }

        std::optional<IndexDefinition> DocEngine::DeserializeIndex(const std::string& bytes) {
            if (bytes.empty()) return std::nullopt;
            size_t pos = 0;
            IndexDefinition idx;
            if (bytes.size() >= 4 && (bytes.compare(0,4,"NXI2")==0 || bytes.compare(0,4,"NXI3")==0)) {
                pos = 4;
                if (!serial::ReadString(bytes, pos, idx.index_id))
                    return std::nullopt;
                idx.format_version = bytes[3]=='3' ? 3 : 2;
            } else {
                if(bytes.starts_with("NXI")) return std::nullopt;
                idx.format_version = 1;
            }
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
            if (pos < bytes.size()) {
                uint8_t state = 0;
                if (!serial::ReadByte(bytes, pos, state)) return std::nullopt;
                idx.state = static_cast<IndexState>(state);
                if (!serial::ReadString(bytes, pos, idx.build_cursor)) return std::nullopt;
                if (!serial::ReadString(bytes, pos, idx.last_error)) return std::nullopt;
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
                const TransactionSettings& transaction_settings,const RocksSettings& rocks_settings)
                : db_path_(db_path),
                  rocks_settings_(rocks_settings),
                  rocks_resources_(rocks_settings.shared_resources ? rocks_settings.shared_resources :
                      std::make_shared<RocksResources>(rocks_settings.memory_budget_bytes,rocks_settings.block_cache_bytes)),
                  txn_db_(nullptr, [this](rocksdb::TransactionDB* db) {
                      if (db) {
                          uint64_t background_errors=0;
                          bool failed=!db->GetIntProperty("rocksdb.background-errors",&background_errors) || background_errors!=0;
                          {std::lock_guard lock(operations_.mutex); failed|=operations_.io_error || operations_.corruption;}
                          // RocksDB 8.5 debug builds assert on SyncWAL after a
                          // previous WAL append error. Do not retry a failed WAL
                          // implicitly during destruction or claim a clean sync.
                          if(!failed) (void)db->SyncWAL();
                          delete db;
                      }
                  }),db_options_(MakeRocksOptions(rocks_settings,rocks_resources_)),
                  write_options_(MakeDocumentWriteOptions(rocks_settings.durability)),read_options_(MakeDocumentReadOptions()) {


            // Immutable profile-specific writes; reads verify checksums. Customize
            // per-call snapshots, bounds and cache policy on local copies only.

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

            std::string stored_document_format;
            s = txn_db_->Get(
                    read_options_,
                    keys::kDocumentFormat,
                    &stored_document_format);
            if (s.ok() && stored_document_format != "1" && stored_document_format != "2" && stored_document_format != "3") {
                throw std::runtime_error(
                        "[NexoraDB] Unsupported document format version: " +
                        stored_document_format);
            }
            if (!s.ok() && !s.IsNotFound()) {
                throw std::runtime_error(
                        "[NexoraDB] Failed to read document format metadata: " +
                        s.ToString());
            }
            ValidateFormatManifest();
            {
                std::unique_ptr<rocksdb::Iterator> indexes(txn_db_->NewIterator(read_options_));
                for(indexes->Seek("meta:idx:");indexes->Valid() && indexes->key().starts_with("meta:idx:");indexes->Next()) {
                    const auto index=DeserializeIndex(indexes->value().ToString());
                    if(!index || index->format_version!=indexv2::kFormatVersion)
                        throw std::runtime_error("Legacy/unknown index format; use offline copy migration before opening this engine");
                    if(stored_document_format!="3") throw std::runtime_error("v3 index requires generation-3 reader fence");
                }
                if(!indexes->status().ok()) throw std::runtime_error(indexes->status().ToString());
            }
            if (!EnsureInternalCollections()) {
                throw std::runtime_error("[NexoraDB] Failed to initialize internal collections");
            }
            {
                std::unique_ptr<rocksdb::Iterator> iterator(txn_db_->NewIterator(read_options_));
                iterator->Seek("meta:fk:");
                has_foreign_keys_=iterator->Valid() && iterator->key().starts_with("meta:fk:");
                if(!iterator->status().ok()) throw std::runtime_error(iterator->status().ToString());
            }

            TouchUserActivity();
            document_migration_worker_ = std::jthread(
                    [this](std::stop_token stop_token) {
                        RunDocumentMigrationWorker(stop_token);
                    });
            page_reaper_worker_ = std::jthread([this](std::stop_token stop) {
                while(!stop.stop_requested()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    if(stop.stop_requested()) break;
                    std::lock_guard lock(page_sessions_mutex_);
                    PrunePageSessionsLocked();
                }
            });

            NX_LOG("DocEngine opened at: " << db_path_);
        }

        DocEngine::~DocEngine() {
            page_reaper_worker_.request_stop();
            if(page_reaper_worker_.joinable()) page_reaper_worker_.join();
            document_migration_worker_.request_stop();
            if (document_migration_worker_.joinable()) {
                document_migration_worker_.join();
            }
            std::lock_guard<std::mutex> lock(page_sessions_mutex_);
            page_sessions_.clear();
            NX_LOG("DocEngine closing at: " << db_path_);
        }

        bool DocEngine::IsHealthy() const noexcept {
            try { return nlohmann::json::parse(HealthReport()).at("ready").get<bool>(); }
            catch(...) { return false; }
        }

        void DocEngine::SetMutationFaultPointForTesting(
                MutationFaultPoint point, void (*on_fault)() noexcept) noexcept {
            mutation_fault_callback_.store(on_fault, std::memory_order_relaxed);
            mutation_fault_point_.store(point, std::memory_order_release);
        }

        void DocEngine::PauseIndexBuildAfterChunksForTesting(
                std::uint32_t chunks) noexcept {
            pause_index_build_chunks_.store(chunks, std::memory_order_release);
        }

        rocksdb::Status DocEngine::InjectMutationFaultIfRequested(
                MutationFaultPoint point) noexcept {
            MutationFaultPoint expected = point;
            if (mutation_fault_point_.compare_exchange_strong(
                    expected,
                    MutationFaultPoint::None,
                    std::memory_order_acq_rel)) {
                if (const auto callback = mutation_fault_callback_.load(std::memory_order_relaxed)) callback();
                return rocksdb::Status::IOError(
                        "Injected mutation fault at stage " +
                        std::to_string(static_cast<unsigned>(point)));
            }
            return rocksdb::Status::OK();
        }

        rocksdb::Status DocEngine::EncodeDocumentForStorage(
                const std::string& json_document,
                std::string& stored_document) {
            auto encoded = DocumentCodec::EncodeJson(json_document);
            if (!encoded.success) {
                return rocksdb::Status::InvalidArgument(
                        "document encoding failed: " + encoded.error);
            }
            stored_document = std::move(encoded.value);
            return rocksdb::Status::OK();
        }

        rocksdb::Status DocEngine::DecodeStoredDocument(
                std::string_view stored_document,
                std::string& json_document) {
            auto decoded = DocumentCodec::DecodeToJson(stored_document);
            if (!decoded.success) {
                return rocksdb::Status::Corruption(
                        "document decoding failed: " + decoded.error);
            }
            json_document = std::move(decoded.value);
            return rocksdb::Status::OK();
        }

        void DocEngine::TouchUserActivity() const noexcept {
            const auto now = std::chrono::steady_clock::now()
                    .time_since_epoch();
            last_user_activity_ns_.store(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(now)
                            .count(),
                    std::memory_order_release);
        }

        rocksdb::Status DocEngine::MigrateLegacyDocumentsChunk(
                std::size_t max_documents,
                std::size_t max_bytes,
                std::size_t& migrated,
                bool& complete) {
            std::lock_guard<std::mutex> migration_lock(
                    document_migration_mutex_);
            migrated = 0;
            complete = false;

            std::string format;
            rocksdb::Status status = txn_db_->Get(
                    read_options_, keys::kDocumentFormat, &format);
            if (status.ok() && (format == "1" || format == "2" || format == "3")) {
                complete = true;
                return rocksdb::Status::OK();
            }
            if (!status.ok() && !status.IsNotFound()) return status;

            std::string cursor;
            status = txn_db_->Get(
                    read_options_, keys::kDocumentMigrationCursor, &cursor);
            if (!status.ok() && !status.IsNotFound()) return status;
            if (status.IsNotFound()) cursor.clear();

            const std::string prefix = keys::kData;
            const std::string upper_value = PrefixUpperBound(prefix);
            const rocksdb::Slice upper_bound(upper_value);
            rocksdb::ReadOptions scan_options = MakeScanReadOptions(read_options_);
            scan_options.fill_cache = false;
            scan_options.iterate_upper_bound = &upper_bound;
            auto iterator = std::unique_ptr<rocksdb::Iterator>(
                    txn_db_->NewIterator(scan_options));
            if (cursor.empty()) {
                iterator->Seek(prefix);
            } else {
                iterator->Seek(cursor);
                if (iterator->Valid() && iterator->key() == cursor) {
                    iterator->Next();
                }
            }

            std::vector<std::string> legacy_keys;
            std::size_t selected_bytes = 0;
            std::string last_scanned_key;
            bool stopped_at_limit = false;
            for (; iterator->Valid(); iterator->Next()) {
                const std::string current_key = iterator->key().ToString();
                const std::string_view current_value(
                        iterator->value().data(), iterator->value().size());
                if (DocumentCodec::Detect(current_value) !=
                    DocumentCodec::Format::LegacyJson) {
                    last_scanned_key = current_key;
                    continue;
                }

                const std::size_t item_bytes = iterator->key().size() +
                                               iterator->value().size();
                if (!legacy_keys.empty() &&
                    (legacy_keys.size() >= max_documents ||
                     selected_bytes + item_bytes > max_bytes)) {
                    stopped_at_limit = true;
                    break;
                }
                legacy_keys.push_back(current_key);
                selected_bytes += item_bytes;
                last_scanned_key = current_key;
                if (legacy_keys.size() >= max_documents) {
                    stopped_at_limit = true;
                    break;
                }
            }
            if (!iterator->status().ok()) return iterator->status();

            // Selected keys are owned. Do not pin old memtables while committing.
            iterator.reset();
            std::unique_ptr<rocksdb::Transaction> transaction(
                    txn_db_->BeginTransaction(
                            write_options_, transaction_options_));
            if (!transaction) {
                return rocksdb::Status::IOError(
                        "unable to start document migration transaction");
            }

            for (const auto& key : legacy_keys) {
                std::string current;
                status = transaction->GetForUpdate(
                        read_options_, key, &current);
                if (status.IsNotFound()) continue;
                if (!status.ok()) return status;
                if (DocumentCodec::Detect(current) !=
                    DocumentCodec::Format::LegacyJson) {
                    continue;
                }

                std::string encoded;
                status = EncodeDocumentForStorage(current, encoded);
                if (!status.ok()) return status;
                status = transaction->Put(key, encoded);
                if (!status.ok()) return status;
                ++migrated;
            }

            if (!stopped_at_limit) {
                status = transaction->Put(keys::kDocumentFormat, "1");
                if (!status.ok()) return status;
                status = transaction->Delete(keys::kDocumentMigrationCursor);
                if (!status.ok()) return status;
                complete = true;
            } else {
                status = transaction->Put(
                        keys::kDocumentMigrationCursor, last_scanned_key);
                if (!status.ok()) return status;
            }

            return transaction->Commit();
        }

        DBResult DocEngine::MigrateLegacyDocuments(
                std::size_t max_documents,
                std::size_t max_bytes) {
            if (max_documents == 0 || max_bytes == 0) {
                return DBResult::Err(
                        "Migration chunk limits must be greater than zero");
            }
            std::size_t migrated = 0;
            bool complete = false;
            const rocksdb::Status status = MigrateLegacyDocumentsChunk(
                    max_documents, max_bytes, migrated, complete);
            if (!status.ok()) {
                return RocksFailure("document migration", status);
            }
            return DBResult::Ok(
                    "{\"migrated\":" + std::to_string(migrated) +
                    ",\"complete\":" +
                    (complete ? "true" : "false") + "}");
        }

        void DocEngine::RunDocumentMigrationWorker(
                std::stop_token stop_token) {
            constexpr auto kIdleWindow = std::chrono::seconds(1);
            auto last_attempt=std::chrono::steady_clock::now();
            while (!stop_token.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (stop_token.stop_requested()) return;

                const auto now = std::chrono::steady_clock::now()
                        .time_since_epoch();
                const auto now_ns =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(now)
                                .count();
                const auto last = last_user_activity_ns_.load(
                        std::memory_order_acquire);
                const bool busy=now_ns - last <
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                            kIdleWindow).count();
                if (busy && std::chrono::steady_clock::now()-last_attempt<std::chrono::seconds(5)) {
                    continue;
                }

                std::size_t migrated = 0;
                bool complete = false;
                last_attempt=std::chrono::steady_clock::now();
                if(busy) ++migration_foreground_grants_;
                const rocksdb::Status status = MigrateLegacyDocumentsChunk(
                        busy ? 100 : 1000, busy ? 256U*1024U : 4U*1024U*1024U, migrated, complete);
                if (!status.ok()) {
                    NX_LOG("Document migration paused: " << status.ToString());
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }
                if (complete) return;

                // Yield between chunks. A foreground request updates the activity
                // timestamp and postpones the next chunk.
                std::this_thread::yield();
            }
        }

// ══════════════════════════════════════════════════════════════
// §5  مدیریت Collection و Schema
// ══════════════════════════════════════════════════════════════

        DBResult DocEngine::CreateCollection(
                const std::string& collection_name,
                const std::optional<SchemaDefinition>& schema) {
            MetadataChange metadata_change(*this,collection_name);
            std::unique_lock ddl_lock(index_ddl_mutex_);

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
            if(std::any_of(eff_schema.fields.begin(),eff_schema.fields.end(),[](const auto& field){return field.unique;})) {
                const auto fence=EnsureIndexFormatFence();
                if(!fence.ok()) return RocksFailure("index format fence",fence);
            }
            rocksdb::WriteBatch batch;
            batch.Put(meta_key, SerializeSchema(eff_schema));
            batch.Put(std::string(keys::kSeq) + collection_name, "0");
            for (const auto& field : eff_schema.fields) {
                if (!field.unique) continue;
                IndexDefinition unique_index;
                unique_index.index_name = "__schema_unique_" + field.name;
                unique_index.fields = {field.name};
                unique_index.type = IndexType::Unique;
                unique_index.index_id = GenerateDocId();
                unique_index.format_version = indexv2::kFormatVersion;
                batch.Put(MakeMetaKey(keys::kMetaIdx, collection_name,
                                      unique_index.index_name),
                          SerializeIndex(unique_index));
            }
            s = txn_db_->Write(write_options_, &batch);
            ROCKS_CHECK(s, "CreateCollection metadata/counter");

            NX_LOG("Collection created: " << collection_name);
            return DBResult::Ok("Collection '" + collection_name + "' created");
        }

        DBResult DocEngine::DropCollection(const std::string& collection_name) {
            MetadataChange metadata_change(*this,collection_name);
            std::unique_lock ddl_lock(index_ddl_mutex_);
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");
            if (!CollectionExists(collection_name))
                return DBResult::Err("Collection '" + collection_name + "' does not exist");

            const auto referenced=CheckForeignKeyDdl(collection_name);
            if(!referenced.ok()) return RocksFailure("DropCollection foreign key",referenced);
            // All foreground writers/transactions take the shared DDL gate.
            // Migration writes use their own mutex; exclude those as well before
            // using TransactionDB's explicitly non-conflicting range-write API.
            std::lock_guard migration_lock(document_migration_mutex_);
            rocksdb::WriteBatch batch;
            // Constant-sized atomic batch, not one delete/lock per document.
            // RocksDB reclaims obsolete SST data later through compaction.
            const std::vector<std::string> prefixes = {
                    std::string(keys::kData) + collection_name + ":",
                    std::string(keys::kIdx) + collection_name + ":",
                    indexv2::CollectionPrefix(collection_name),
                    std::string(keys::kMetaIdx) + collection_name + ":",
                    std::string(keys::kMetaFk) + collection_name + ":"};
            for (const auto& prefix : prefixes) {
                const auto upper = PrefixUpperBound(prefix);
                if (upper.empty()) return DBResult::Err("DropCollection invalid key range");
                const auto status = batch.DeleteRange(prefix, upper);
                if (!status.ok()) return RocksFailure("DropCollection range staging", status);
            }

            batch.Delete(MakeMetaKey(keys::kMetaCol, collection_name));
            batch.Delete(std::string(keys::kSeq) + collection_name);

            rocksdb::TransactionDBWriteOptimizations optimizations;
            optimizations.skip_concurrency_control = true; // Exclusivity proven by gates above.
            rocksdb::Status s = txn_db_->Write(write_options_, optimizations, &batch);
            ROCKS_CHECK(s, "DropCollection");

            NX_LOG("Collection dropped: " << collection_name);
            return DBResult::Ok("Collection '" + collection_name + "' dropped");
        }

        DBResult DocEngine::SetSchemaValidation(const std::string&      collection_name,
                                                const SchemaDefinition& schema) {
            MetadataChange metadata_change(*this,collection_name);
            std::unique_lock<std::shared_mutex> ddl_lock(index_ddl_mutex_);
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");
            if (!CollectionExists(collection_name))
                return DBResult::Err("Collection '" + collection_name + "' does not exist");
            const auto indexes = GetIndexes(collection_name);
            for (const auto& field : schema.fields) {
                if (!field.unique) continue;
                const bool enforced = std::any_of(
                        indexes.begin(), indexes.end(),
                        [&](const IndexDefinition& index) {
                            if (index.state != IndexState::Ready) return false;
                            return index.type == IndexType::Unique &&
                                   index.fields.size() == 1 &&
                                   index.fields.front() == field.name;
                        });
                if (!enforced)
                    return DBResult::Err(
                            "SchemaField::unique requires a Unique index for field '" +
                            field.name + "'");
            }
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
            MetadataChange metadata_change(*this,collection_name);
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");
            if (!CollectionExists(collection_name))
                return DBResult::Err("Collection '" + collection_name + "' does not exist");
            if (index_def.index_name.empty() || index_def.fields.empty())
                return DBResult::Err("Index must have a name and at least one field");

            IndexDefinition effective_index = index_def;
            // Physical identity is always engine-owned; callers cannot inject
            // or accidentally reuse an ID belonging to another index.
            effective_index.index_id = GenerateDocId();
            effective_index.format_version = indexv2::kFormatVersion;
            effective_index.state = IndexState::Building;
            effective_index.build_cursor.clear();
            effective_index.last_error.clear();
            std::string meta_key = MakeMetaKey(keys::kMetaIdx, collection_name,
                                               effective_index.index_name);
            {
                std::unique_lock<std::shared_mutex> ddl_lock(index_ddl_mutex_);
                std::string existing;
                rocksdb::Status status = txn_db_->Get(
                        read_options_, meta_key, &existing);
                if (status.ok())
                    return DBResult::Err("Index '" + index_def.index_name + "' already exists");
                if (!status.IsNotFound())
                    return RocksFailure("CreateIndex metadata lookup", status);
                status=EnsureIndexFormatFence();
                if(!status.ok()) return RocksFailure("index format fence",status);
                status = txn_db_->Put(write_options_, meta_key,
                                      SerializeIndex(effective_index));
                if (!status.ok())
                    return RocksFailure("CreateIndex publish Building", status);
            }
            return ResumeIndexBuild(collection_name, effective_index.index_name);
        }

        DBResult DocEngine::DropIndex(const std::string& collection_name,
                                      const std::string& index_name) {
            MetadataChange metadata_change(*this,collection_name);
            std::unique_lock<std::shared_mutex> ddl_lock(index_ddl_mutex_);
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");
            std::string meta_key = MakeMetaKey(keys::kMetaIdx, collection_name, index_name);
            std::string ser;
            rocksdb::Status s = txn_db_->Get(read_options_, meta_key, &ser);
            if (!s.ok()) return DBResult::Err("Index '" + index_name + "' not found");

            auto idx_def = DeserializeIndex(ser);
            if (!idx_def) return DBResult::Err("Corrupted index metadata");
            for(const auto& field:idx_def->fields) {
                const auto referenced=CheckForeignKeyDdl(collection_name,&field);
                if(!referenced.ok()) return RocksFailure("DropIndex foreign key",referenced);
            }

            const auto schema = GetSchema(collection_name);
            if (schema && idx_def->type == IndexType::Unique &&
                idx_def->fields.size() == 1) {
                const auto schema_field = std::find_if(
                        schema->fields.begin(), schema->fields.end(),
                        [&](const SchemaField& field) {
                            return field.name == idx_def->fields.front() &&
                                   field.unique;
                        });
                if (schema_field != schema->fields.end())
                    return DBResult::Err(
                            "Cannot drop index enforcing SchemaField::unique");
            }

            rocksdb::WriteBatch batch;
            rocksdb::ReadOptions ro=MakeScanReadOptions(read_options_);
            auto it = std::unique_ptr<rocksdb::Iterator>(txn_db_->NewIterator(ro));
            if (idx_def->format_version == indexv2::kFormatVersion &&
                !idx_def->index_id.empty()) {
                const std::string prefix = indexv2::IndexPrefix(
                        collection_name, idx_def->index_id);
                for (it->Seek(prefix);
                     it->Valid() && it->key().starts_with(prefix);
                     it->Next())
                    batch.Delete(it->key());
            } else {
                for (const auto& field : idx_def->fields) {
                    const std::string prefix = std::string(keys::kIdx) +
                            collection_name + ":" + field + ":";
                    for (it->Seek(prefix);
                         it->Valid() && it->key().starts_with(prefix);
                         it->Next())
                        batch.Delete(it->key());
                }
            }
            if (!it->status().ok())
                return RocksFailure("DropIndex scan", it->status());
            batch.Delete(meta_key);

            s = txn_db_->Write(write_options_, &batch);
            ROCKS_CHECK(s, "DropIndex");
            return DBResult::Ok("Index '" + index_name + "' dropped");
        }

        DBResult DocEngine::RebuildIndexes(const std::string& collection_name) {
            MetadataChange metadata_change(*this,collection_name);
            std::unique_lock<std::shared_mutex> ddl_lock(index_ddl_mutex_);
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");
            if (!CollectionExists(collection_name))
                return DBResult::Err("Collection '" + collection_name + "' does not exist");

            auto indexes = GetIndexes(collection_name);
            rocksdb::WriteBatch batch;
            auto iterator = std::unique_ptr<rocksdb::Iterator>(
                    txn_db_->NewIterator(MakeScanReadOptions(read_options_)));

            for (auto& index : indexes) {
                if (index.format_version == indexv2::kFormatVersion &&
                    !index.index_id.empty()) {
                    const std::string prefix = indexv2::IndexPrefix(
                            collection_name, index.index_id);
                    for (iterator->Seek(prefix);
                         iterator->Valid() && iterator->key().starts_with(prefix);
                         iterator->Next()) batch.Delete(iterator->key());
                } else {
                    for (const auto& field : index.fields) {
                        const std::string prefix = std::string(keys::kIdx) +
                                collection_name + ":" + field + ":";
                        for (iterator->Seek(prefix);
                             iterator->Valid() && iterator->key().starts_with(prefix);
                             iterator->Next()) batch.Delete(iterator->key());
                    }
                    index.index_id = GenerateDocId();
                    index.format_version = indexv2::kFormatVersion;
                }
                index.state = IndexState::Ready;
                index.build_cursor.clear();
                index.last_error.clear();

                std::unordered_map<std::string, std::string> unique_owners;
                const std::string data_prefix = std::string(keys::kData) +
                        collection_name + ":";
                for (iterator->Seek(data_prefix);
                     iterator->Valid() && iterator->key().starts_with(data_prefix);
                     iterator->Next()) {
                    const std::string doc_id = iterator->key().ToString().substr(
                            data_prefix.size());
                    const auto tuple = indexv2::EncodeTuple(
                            iterator->value().ToString(), index.fields);
                    if (!tuple) continue;
                    if (index.type == IndexType::Unique) {
                        const std::string key = indexv2::UniqueKey(
                                collection_name, index.index_id, *tuple);
                        if (!unique_owners.emplace(key, doc_id).second)
                            return DBResult::Err(
                                    "Unique index duplicate while rebuilding '" +
                                    index.index_name + "'");
                        batch.Put(key, doc_id);
                    }
                    batch.Put(indexv2::EntryKey(collection_name, index.index_id,
                                                *tuple, doc_id), doc_id);
                }
                batch.Put(MakeMetaKey(keys::kMetaIdx, collection_name,
                                      index.index_name),
                          SerializeIndex(index));
            }
            if (!iterator->status().ok())
                return RocksFailure("RebuildIndexes scan", iterator->status());
            const rocksdb::Status status = txn_db_->Write(write_options_, &batch);
            if (!status.ok()) return RocksFailure("RebuildIndexes commit", status);
            return DBResult::Ok(std::to_string(indexes.size()));
        }

        DBResult DocEngine::CleanupIndexBuild(
                const std::string& collection_name,
                const std::string& index_name) {
            MetadataChange metadata_change(*this,collection_name);
            std::unique_lock<std::shared_mutex> ddl_lock(index_ddl_mutex_);
            const std::string meta_key = MakeMetaKey(
                    keys::kMetaIdx, collection_name, index_name);
            std::string serialized;
            rocksdb::Status status = txn_db_->Get(
                    read_options_, meta_key, &serialized);
            if (!status.ok()) return RocksFailure("CleanupIndexBuild metadata", status);
            auto index = DeserializeIndex(serialized);
            if (!index) return DBResult::Err("Corrupted index metadata");
            if (index->state == IndexState::Ready)
                return DBResult::Err("Cannot cleanup a Ready index");

            const std::string prefix = indexv2::IndexPrefix(
                    collection_name, index->index_id);
            while (true) {
                rocksdb::WriteBatch batch;
                std::size_t count = 0;
                auto iterator = std::unique_ptr<rocksdb::Iterator>(
                        txn_db_->NewIterator(MakeScanReadOptions(read_options_)));
                for (iterator->Seek(prefix);
                     iterator->Valid() && iterator->key().starts_with(prefix) &&
                     count < 1000;
                     iterator->Next(), ++count)
                    batch.Delete(iterator->key());
                if (!iterator->status().ok())
                    return RocksFailure("CleanupIndexBuild scan", iterator->status());
                if (count == 0) break;
                status = txn_db_->Write(write_options_, &batch);
                if (!status.ok())
                    return RocksFailure("CleanupIndexBuild delete chunk", status);
            }
            index->build_cursor.clear();
            status = txn_db_->Put(write_options_, meta_key, SerializeIndex(*index));
            if (!status.ok()) return RocksFailure("CleanupIndexBuild checkpoint", status);
            return DBResult::Ok("Index build artifacts cleaned");
        }

        DBResult DocEngine::ResumeIndexBuild(
                const std::string& collection_name,
                const std::string& index_name) {
            MetadataChange metadata_change(*this,collection_name);
            const std::string meta_key = MakeMetaKey(
                    keys::kMetaIdx, collection_name, index_name);
            std::string serialized;
            rocksdb::Status status = txn_db_->Get(
                    read_options_, meta_key, &serialized);
            if (!status.ok()) return RocksFailure("ResumeIndexBuild metadata", status);
            auto decoded = DeserializeIndex(serialized);
            if (!decoded) return DBResult::Err("Corrupted index metadata");
            IndexDefinition index = *decoded;
            if (index.state == IndexState::Ready)
                return DBResult::Ok("Index is already Ready");
            if (index.state == IndexState::Failed) {
                const DBResult cleanup = CleanupIndexBuild(
                        collection_name, index_name);
                if (!cleanup.success) return cleanup;
                index.state = IndexState::Building;
                index.last_error.clear();
                status = txn_db_->Put(write_options_, meta_key, SerializeIndex(index));
                if (!status.ok()) return RocksFailure("ResumeIndexBuild restart", status);
            }

            auto fail_build = [&](const std::string& message) -> DBResult {
                index.state = IndexState::Failed;
                index.last_error = message;
                const rocksdb::Status stored = txn_db_->Put(
                        write_options_, meta_key, SerializeIndex(index));
                if (!stored.ok())
                    return RocksFailure("ResumeIndexBuild mark Failed", stored);
                const DBResult cleanup = CleanupIndexBuild(
                        collection_name, index_name);
                if (!cleanup.success)
                    return DBResult::Err(message + "; " + cleanup.error_msg);
                return DBResult::Err(message);
            };

            const std::string data_prefix = std::string(keys::kData) +
                    collection_name + ":";
            constexpr std::size_t kMaxDocuments = 1000;
            constexpr std::size_t kMaxBytes = 4 * 1024 * 1024;
            std::uint64_t indexed = 0;

            while (true) {
                std::vector<std::string> keys_to_process;
                std::size_t bytes = 0;
                rocksdb::ReadOptions scan_options=MakeScanReadOptions(read_options_);
                scan_options.fill_cache = false;
                const std::string upper_value = PrefixUpperBound(data_prefix);
                const rocksdb::Slice upper_bound(upper_value);
                scan_options.iterate_upper_bound = &upper_bound;
                auto iterator = std::unique_ptr<rocksdb::Iterator>(
                        txn_db_->NewIterator(scan_options));
                if (index.build_cursor.empty()) iterator->Seek(data_prefix);
                else {
                    iterator->Seek(index.build_cursor);
                    if (iterator->Valid() && iterator->key() == index.build_cursor)
                        iterator->Next();
                }
                for (; iterator->Valid() && keys_to_process.size() < kMaxDocuments;
                     iterator->Next()) {
                    const std::size_t item_bytes = iterator->key().size() +
                                                   iterator->value().size();
                    if (!keys_to_process.empty() && bytes + item_bytes > kMaxBytes)
                        break;
                    keys_to_process.push_back(iterator->key().ToString());
                    bytes += item_bytes;
                }
                if (!iterator->status().ok())
                    return fail_build(iterator->status().ToString());
                if (keys_to_process.empty()) break;

                auto transaction = BeginTransaction();
                if (!transaction)
                    return fail_build("Unable to start index build transaction");
                for (const auto& document_key : keys_to_process) {
                    std::string document;
                    status = transaction->Get()->GetForUpdate(
                            read_options_, document_key, &document);
                    if (status.IsNotFound()) continue;
                    if (!status.ok()) {
                        RollbackTransaction(*transaction);
                        return fail_build(status.ToString());
                    }
                    const std::string doc_id = document_key.substr(data_prefix.size());
                    const auto tuple = indexv2::EncodeTuple(document, index.fields);
                    if (!tuple) continue;
                    if (index.type == IndexType::Unique) {
                        const std::string unique_key = indexv2::UniqueKey(
                                collection_name, index.index_id, *tuple);
                        std::string owner;
                        status = transaction->Get()->GetForUpdate(
                                read_options_, unique_key, &owner);
                        if (status.ok() && owner != doc_id) {
                            RollbackTransaction(*transaction);
                            return fail_build(
                                    "Unique index duplicate while building '" +
                                    index.index_name + "'");
                        }
                        if (!status.ok() && !status.IsNotFound()) {
                            RollbackTransaction(*transaction);
                            return fail_build(status.ToString());
                        }
                        status = transaction->Get()->Put(unique_key, doc_id);
                        if (!status.ok()) {
                            RollbackTransaction(*transaction);
                            return fail_build(status.ToString());
                        }
                    }
                    status = transaction->Get()->Put(
                            indexv2::EntryKey(collection_name, index.index_id,
                                             *tuple, doc_id), doc_id);
                    if (!status.ok()) {
                        RollbackTransaction(*transaction);
                        return fail_build(status.ToString());
                    }
                    ++indexed;
                }
                index.build_cursor = keys_to_process.back();
                status = transaction->Get()->Put(
                        meta_key, SerializeIndex(index));
                if (!status.ok()) {
                    RollbackTransaction(*transaction);
                    return fail_build(status.ToString());
                }
                const DBResult committed = CommitTransaction(*transaction);
                if (!committed.success) return fail_build(committed.error_msg);
                const std::uint32_t remaining = pause_index_build_chunks_.load(
                        std::memory_order_acquire);
                if (remaining > 0 && pause_index_build_chunks_.fetch_sub(
                        1, std::memory_order_acq_rel) == 1)
                    return DBResult::Err("Index build paused for testing");
            }

            {
                std::unique_lock<std::shared_mutex> ddl_lock(index_ddl_mutex_);
                index.state = IndexState::Ready;
                index.build_cursor.clear();
                index.last_error.clear();
                status = txn_db_->Put(write_options_, meta_key, SerializeIndex(index));
            }
            if (!status.ok()) return RocksFailure("ResumeIndexBuild publish Ready", status);
            return DBResult::Ok(std::to_string(indexed));
        }

// ══════════════════════════════════════════════════════════════
// §7  مدیریت Foreign Key
// ══════════════════════════════════════════════════════════════

        DBResult DocEngine::AddForeignKey(const std::string&          collection_name,
                                          const ForeignKeyDefinition& fk_def) {
            MetadataChange metadata_change(*this,collection_name);
            std::unique_lock<std::shared_mutex> ddl_lock(index_ddl_mutex_);
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
                            return index.state == IndexState::Ready &&
                                   !index.fields.empty() &&
                                   index.fields.front() == fk_def.ref_field;
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

            // DDL excludes all writers. Do not publish a constraint over
            // already-invalid rows; audit every existing child first.
            const std::string prefix="data:"+collection_name+":";
            std::unique_ptr<rocksdb::Iterator> children(txn_db_->NewIterator(MakeScanReadOptions(read_options_)));
            for(children->Seek(prefix);children->Valid() && children->key().starts_with(prefix);children->Next()) {
                std::string document,error;
                s=DecodeStoredDocument(children->value().ToString(),document);
                if(s.ok()) s=ValidateForeignKeysChecked({fk_def},document,error);
                if(!s.ok()) return RocksFailure("AddForeignKey existing data",s);
            }
            if(!children->status().ok()) return RocksFailure("AddForeignKey audit",children->status());
            s = txn_db_->Put(write_options_, meta_key, SerializeFk(fk_def));
            ROCKS_CHECK(s, "AddForeignKey");
            has_foreign_keys_=true;
            return DBResult::Ok("Foreign key '" + fk_def.fk_name + "' added");
        }

        DBResult DocEngine::DropForeignKey(const std::string& collection_name,
                                           const std::string& fk_name) {
            MetadataChange metadata_change(*this,collection_name);
            std::unique_lock ddl_lock(index_ddl_mutex_);
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
            uint64_t epoch;
            {
                std::lock_guard lock(metadata_cache_mutex_);
                epoch=metadata_epoch_;
                if (!metadata_writers_) {
                    const auto found=metadata_cache_.find(collection_name);
                    if (found!=metadata_cache_.end()) { metadata=found->second; return rocksdb::Status::OK(); }
                }
            }

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

            rocksdb::ReadOptions iterator_options = MakeScanReadOptions(read_options_);
            {
                PrefixIterator iterator(txn_db_.get(),foreign_key_prefix,iterator_options);

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
                PrefixIterator iterator(txn_db_.get(),index_prefix,iterator_options);

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
            {
                std::lock_guard lock(metadata_cache_mutex_);
                if (!metadata_writers_ && epoch==metadata_epoch_) {
                    if (metadata_cache_.size()>=256) metadata_cache_.clear();
                    metadata_cache_[collection_name]=metadata;
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
            auto encoded_document = DocumentCodec::EncodeJson(bson_document);
            if (!encoded_document.success) {
                return rocksdb::Status::InvalidArgument(
                        "invalid document for FK validation: " +
                        encoded_document.error);
            }
            const query::DocumentView document_view(
                    std::string_view(encoded_document.value).substr(
                            DocumentCodec::kEnvelopeSize));
            if (!document_view.valid()) {
                return rocksdb::Status::Corruption(
                        "invalid BSON view for FK validation");
            }

            for (const auto& foreign_key : foreign_keys) {
                const query::CompiledFieldPath path(foreign_key.local_field);
                if (!path.valid()) {
                    return rocksdb::Status::InvalidArgument(path.error());
                }
                const std::string local_value = ScalarToString(
                        document_view.Get(path));

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

                const auto indexes = GetIndexes(foreign_key.ref_collection);
                const auto matching_index = std::find_if(
                        indexes.begin(), indexes.end(),
                        [&](const IndexDefinition& index) {
                            return index.format_version == indexv2::kFormatVersion &&
                                   index.state == IndexState::Ready &&
                                   !index.fields.empty() &&
                                   index.fields.front() == foreign_key.ref_field;
                        });
                if (matching_index == indexes.end())
                    return rocksdb::Status::InvalidArgument(
                            "FK supporting index is missing or requires migration");
                const auto tuple = indexv2::EncodeTuple(
                        bson_document, {foreign_key.local_field});
                if (!tuple) continue;
                std::string index_prefix = indexv2::IndexPrefix(
                        foreign_key.ref_collection,
                        matching_index->index_id);
                index_prefix.push_back('E');
                index_prefix.append(*tuple);

                if (!index_iterator) {
                    index_iterator.reset(transaction
                            ? transaction->GetIterator(MakeScanReadOptions(read_options_))
                            : txn_db_->NewIterator(MakeScanReadOptions(read_options_)));
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

        rocksdb::Status DocEngine::LockConstraintGuard(rocksdb::Transaction* transaction) const {
            if(!has_foreign_keys_) return rocksdb::Status::OK();
            std::string ignored;
            auto status=transaction->GetForUpdate(read_options_,"meta:lock:fk-constraints",&ignored);
            return status.IsNotFound() ? rocksdb::Status::OK() : status;
        }

        rocksdb::Status DocEngine::CheckForeignKeyDdl(const std::string& collection,const std::string* field) const {
            std::unique_ptr<rocksdb::Iterator> iterator(txn_db_->NewIterator(MakeScanReadOptions(read_options_)));
            for(iterator->Seek("meta:fk:");iterator->Valid() && iterator->key().starts_with("meta:fk:");iterator->Next()) {
                const auto fk=DeserializeFk(iterator->value().ToString());
                if(!fk) return rocksdb::Status::Corruption("invalid foreign key metadata");
                if(fk->ref_collection==collection && (!field || fk->ref_field==*field))
                    return rocksdb::Status::InvalidArgument("referenced by foreign key; drop constraint explicitly first: "+fk->fk_name);
            }
            return iterator->status();
        }

        rocksdb::Status DocEngine::CheckIncomingReferences(const std::string& collection,
                const std::string& id,const std::string& old_document,const std::string* replacement,
                rocksdb::Transaction* transaction) const {
            if(!has_foreign_keys_) return rocksdb::Status::OK();
            std::unique_ptr<rocksdb::Iterator> metadata(transaction->GetIterator(MakeScanReadOptions(read_options_)));
            for(metadata->Seek("meta:fk:");metadata->Valid() && metadata->key().starts_with("meta:fk:");metadata->Next()) {
                const auto fk=DeserializeFk(metadata->value().ToString());
                if(!fk) return rocksdb::Status::Corruption("invalid foreign key metadata");
                if(fk->ref_collection!=collection) continue;
                const auto old_tuple=indexv2::EncodeTuple(old_document,{fk->ref_field});
                if(replacement && old_tuple==indexv2::EncodeTuple(*replacement,{fk->ref_field})) continue;
                if(!old_tuple) continue;
                const auto key=metadata->key().ToString();
                const std::string suffix=":"+fk->fk_name;
                if(!key.ends_with(suffix) || key.size()<=8+suffix.size()) return rocksdb::Status::Corruption("invalid FK key");
                const auto child_collection=key.substr(8,key.size()-8-suffix.size()),prefix="data:"+child_collection+":";
                std::unique_ptr<rocksdb::Iterator> children(transaction->GetIterator(MakeScanReadOptions(read_options_)));
                for(children->Seek(prefix);children->Valid() && children->key().starts_with(prefix);children->Next()) {
                    if(!replacement && child_collection==collection && children->key().ToString()==prefix+id) continue;
                    const auto stored=children->value().ToString();
                    bool matches=false;
                    if(fk->ref_field=="_id") {
                        // Decode to JSON here: this is a correctness-first FK
                        // RESTRICT scan, not the no-FK ingestion hot path.
                        std::string json;auto status=DecodeStoredDocument(stored,json);
                        if(!status.ok()) return status;
                        auto bytes=DocumentCodec::EncodeJson(json);
                        if(!bytes.success) return rocksdb::Status::Corruption(bytes.error);
                        query::DocumentView view(std::string_view(bytes.value).substr(DocumentCodec::kEnvelopeSize));
                        matches=ScalarToString(view.Get(query::CompiledFieldPath(fk->local_field)))==id;
                    } else matches=indexv2::EncodeTuple(stored,{fk->local_field})==old_tuple;
                    if(matches) return rocksdb::Status::InvalidArgument("FK RESTRICT violation: "+fk->fk_name);
                }
                if(!children->status().ok()) return children->status();
            }
            return metadata->status();
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
                PrepareIndexes();
                owned_transaction_.reset(
                        engine_.txn_db_->BeginTransaction(
                                engine_.write_options_,
                                engine_.transaction_options_));
                transaction_ = owned_transaction_.get();
                if(transaction_) constraint_status_=engine_.LockConstraintGuard(transaction_);
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
                PrepareIndexes();
                transaction_->SetSavePoint();
                constraint_status_=engine_.LockConstraintGuard(transaction_);
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
                if(!constraint_status_.ok()) return constraint_status_;
                if (!transaction_) {
                    return rocksdb::Status::InvalidArgument(
                            "Mutation transaction is not initialized");
                }

                std::string stored_document;
                rocksdb::Status status = transaction_->GetForUpdate(
                        engine_.read_options_,
                        DocEngine::MakeDocKey(
                                collection_,
                                document_id),
                        &stored_document);
                if (!status.ok()) return status;
                return DocEngine::DecodeStoredDocument(
                        stored_document, document);
            }

            rocksdb::Status PutDocument(
                    const std::string& document_id,
                    const std::string& bson_document) {
                std::string stored_document;
                rocksdb::Status status =
                        DocEngine::EncodeDocumentForStorage(
                                bson_document, stored_document);
                if (!status.ok()) return status;

                return PutEncodedDocument(document_id, stored_document);
            }

            // Accept only storage bytes produced by DocumentCodec in this engine.
            // Reuse them for index extraction instead of parsing JSON per index.
            rocksdb::Status PutEncodedDocument(
                    const std::string& document_id,
                    const std::string& stored_document) {
                ReserveStaging(stored_document.size()+document_id.size());
                rocksdb::Status status=constraint_status_;
                if(!status.ok()) return status;
                if(engine_.has_foreign_keys_) {
                    MutationMetadata metadata;std::string json,error;
                    status=engine_.LoadMutationMetadata(collection_,metadata);
                    if(!status.ok()) return status;
                    status=DocEngine::DecodeStoredDocument(stored_document,json);
                    if(status.ok()) status=engine_.ValidateForeignKeysChecked(metadata.foreign_keys,json,error,transaction_);
                    if(!status.ok()) return status;
                }

                status = transaction_->Put(
                        DocEngine::MakeDocKey(
                                collection_,
                                document_id),
                        stored_document);

                if (!status.ok()) {
                    return status;
                }

                status = engine_.InjectMutationFaultIfRequested(
                        MutationFaultPoint::AfterDocument);
                if (!status.ok()) return status;

                status = PutIndexEntries(
                        document_id,
                        stored_document);
                if (!status.ok()) return status;

                return engine_.InjectMutationFaultIfRequested(
                        MutationFaultPoint::AfterIndexes);
            }

            rocksdb::Status ReplaceDocument(
                    const std::string& document_id,
                    const std::string& old_document,
                    const std::string& new_document) {
                ReserveStaging(old_document.size()+new_document.size()+document_id.size());
                if(!constraint_status_.ok()) return constraint_status_;
                auto incoming=engine_.CheckIncomingReferences(collection_,document_id,old_document,&new_document,transaction_);
                if(!incoming.ok()) return incoming;
                if(engine_.has_foreign_keys_) {
                    MutationMetadata metadata;std::string error;
                    auto status=engine_.LoadMutationMetadata(collection_,metadata);
                    if(status.ok()) status=engine_.ValidateForeignKeysChecked(metadata.foreign_keys,new_document,error,transaction_);
                    if(!status.ok()) return status;
                }
                std::string stored_document;
                rocksdb::Status status =
                        DocEngine::EncodeDocumentForStorage(
                                new_document, stored_document);
                if (!status.ok()) return status;

                std::string old_stored;
                if (!indexes_.empty()) {
                    status=DocEngine::EncodeDocumentForStorage(old_document,old_stored);
                    if (!status.ok()) return status;
                }
                status = DeleteIndexEntries(
                        document_id,
                        old_stored, &stored_document);

                if (!status.ok()) {
                    return status;
                }

                status = transaction_->Put(
                        DocEngine::MakeDocKey(
                                collection_,
                                document_id),
                        stored_document);

                if (!status.ok()) {
                    return status;
                }

                status = engine_.InjectMutationFaultIfRequested(MutationFaultPoint::AfterDocument);
                if (!status.ok()) return status;
                status = PutIndexEntries(
                        document_id,
                        stored_document, &old_stored);
                if (!status.ok()) return status;
                return engine_.InjectMutationFaultIfRequested(MutationFaultPoint::AfterIndexes);
            }

            rocksdb::Status DeleteDocument(
                    const std::string& document_id,
                    const std::string& old_document) {
                ReserveStaging(old_document.size()+document_id.size());
                if(!constraint_status_.ok()) return constraint_status_;
                const auto incoming=engine_.CheckIncomingReferences(collection_,document_id,old_document,nullptr,transaction_);
                if(!incoming.ok()) return incoming;
                rocksdb::Status status = DeleteIndexEntries(
                        document_id,
                        old_document);

                if (!status.ok()) {
                    return status;
                }

                status = engine_.InjectMutationFaultIfRequested(MutationFaultPoint::AfterIndexes);
                if (!status.ok()) return status;
                status = transaction_->Delete(
                        DocEngine::MakeDocKey(
                                collection_,
                                document_id));
                if (!status.ok()) return status;
                return engine_.InjectMutationFaultIfRequested(MutationFaultPoint::AfterDocument);
            }

            rocksdb::Status AdjustCounter(std::int64_t delta) {
                const std::string counter_key =
                        std::string(keys::kSeq) + collection_;

                std::string stored_value;

                rocksdb::Status status;
                {
                    WriteStageTimer timer(engine_.operations_,OperationsState::CounterLookupStage);
                    status = transaction_->GetForUpdate(engine_.read_options_,counter_key,&stored_value);
                }

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

                if (parsed_characters != stored_value.size() || current_value < 0 ||
                    stored_value.find_first_not_of("0123456789") != std::string::npos) {
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
                    return rocksdb::Status::Corruption(
                            "Document counter underflow for collection: " + collection_);
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
                CheckExecution(); // Last cancellable point; never check after Commit.
                if(!constraint_status_.ok()) return constraint_status_;
                if (!transaction_) {
                    return rocksdb::Status::InvalidArgument(
                            "Mutation transaction is not initialized");
                }

                WriteStageTimer timer(engine_.operations_,OperationsState::CommitStage,!uses_savepoint_);
                const rocksdb::Status status = uses_savepoint_
                        ? transaction_->PopSavePoint()
                        : transaction_->Commit();

                if (status.ok()) {
                    finished_ = true;
                } else {
                    // Commit failure must poison health even for entry points
                    // without an Observe wrapper (UpdateMany/DeleteMany/internal writes).
                    std::lock_guard lock(engine_.operations_.mutex);
                    engine_.operations_.io_error |= status.IsIOError();
                    engine_.operations_.corruption |= status.IsCorruption();
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
            struct PreparedIndex {
                std::string prefix;
                std::vector<query::CompiledFieldPath> paths;
            };
            std::vector<PreparedIndex> prepared_indexes_;
            void PrepareIndexes() {
                prepared_indexes_.reserve(indexes_.size());
                for (const auto& index : indexes_) {
                    PreparedIndex prepared;
                    prepared.prefix=indexv2::IndexPrefix(collection_,index.index_id);
                    prepared.paths.reserve(index.fields.size());
                    for (const auto& field : index.fields) prepared.paths.emplace_back(field);
                    prepared_indexes_.push_back(std::move(prepared));
                }
            }
            rocksdb::Status PutIndexEntries(
                    const std::string& document_id,
                    const std::string& bson_document,
                    const std::string* previous_document = nullptr) {
                std::optional<query::DocumentView> view;
                if (DocumentCodec::Detect(bson_document)==DocumentCodec::Format::BsonV1)
                    view.emplace(std::string_view(bson_document).substr(DocumentCodec::kEnvelopeSize));
                for (std::size_t i=0;i<indexes_.size();++i) {
                    const auto& index=indexes_[i];
                    const auto& prepared=prepared_indexes_[i];
                    if (index.state == IndexState::Failed) continue;
                    if (index.format_version != indexv2::kFormatVersion ||
                        index.index_id.empty())
                        return rocksdb::Status::InvalidArgument(
                                "Legacy index requires migration: " +
                                index.index_name);
                    const auto tuple = view ? indexv2::EncodeTuple(*view,prepared.paths) :
                            indexv2::EncodeTuple(bson_document,index.fields);
                    if (index.state==IndexState::Ready && previous_document && tuple==indexv2::EncodeTuple(*previous_document,index.fields))
                        continue; // Index physical key/unique owner is unchanged; keep it locked by document ownership.
                    if (!tuple) continue; // null/missing are not indexed
                    if (index.type == IndexType::Unique) {
                        const std::string unique_key = prepared.prefix + 'U' + *tuple;
                        std::string owner;
                        rocksdb::Status status = transaction_->GetForUpdate(
                                engine_.read_options_, unique_key, &owner);
                        if (status.ok() && owner != document_id)
                            return rocksdb::Status::InvalidArgument(
                                    "Unique constraint violation: " +
                                    index.index_name);
                        if (!status.ok() && !status.IsNotFound()) return status;
                        status = transaction_->Put(unique_key, document_id);
                        if (!status.ok()) return status;
                    }
                    const rocksdb::Status status = transaction_->Put(
                            indexv2::EntryKeyFromPrefix(prepared.prefix,*tuple,document_id),
                            document_id);
                    if (!status.ok()) return status;
                }

                return rocksdb::Status::OK();
            }

            rocksdb::Status DeleteIndexEntries(
                    const std::string& document_id,
                    const std::string& bson_document,
                    const std::string* replacement_document = nullptr) {
                for (const auto& index : indexes_) {
                    if (index.state == IndexState::Failed) continue;
                    if (index.format_version != indexv2::kFormatVersion ||
                        index.index_id.empty())
                        return rocksdb::Status::InvalidArgument(
                                "Legacy index requires migration: " +
                                index.index_name);
                    const auto tuple = indexv2::EncodeTuple(
                            bson_document, index.fields);
                    if (index.state==IndexState::Ready && replacement_document && tuple==indexv2::EncodeTuple(*replacement_document,index.fields))
                        continue;
                    if (!tuple) continue;
                    rocksdb::Status status = transaction_->Delete(
                            indexv2::EntryKey(collection_, index.index_id,
                                             *tuple, document_id));
                    if (!status.ok()) return status;
                    if (index.type == IndexType::Unique) {
                        status = transaction_->Delete(indexv2::UniqueKey(
                                collection_, index.index_id, *tuple));
                        if (!status.ok()) return status;
                    }
                }

                return rocksdb::Status::OK();
            }

            std::shared_ptr<BudgetLease> staging_credit_;
            void ReserveStaging(size_t payload) {
                CheckExecution();
                if(uses_savepoint_) return; // The externally-owned TxHandle retains its own credits.
                const auto factor=std::max<size_t>(16,2*(indexes_.size()+1));
                if(payload>(SIZE_MAX-4096)/factor) throw ResourceExhausted("mutation reservation overflow");
                if(!staging_credit_) staging_credit_=std::make_shared<BudgetLease>(engine_.ExecutionBudget(),ResourceBudget::Buffer);
                const auto charge=payload*factor+4096;
                if(charge>SIZE_MAX-staging_credit_->bytes()) throw ResourceExhausted("mutation reservation overflow");
                staging_credit_->resize(staging_credit_->bytes()+charge);
            }
            DocEngine& engine_;
            std::string collection_;
            const std::vector<IndexDefinition>& indexes_;
            std::unique_ptr<rocksdb::Transaction> owned_transaction_;
            rocksdb::Transaction* transaction_ = nullptr;
            bool uses_savepoint_ = false;
            bool finished_ = false;
            rocksdb::Status constraint_status_;
        };
// ══════════════════════════════════════════════════════════════

// ══════════════════════════════════════════════════════════════
// §8  متدهای کمکی Validation
// ══════════════════════════════════════════════════════════════

        bool DocEngine::ApplyInsertDefaults(std::string& document,
                                           const SchemaDefinition& schema,
                                           std::string& error, std::string* encoded) const {
            auto validated = DocumentCodec::EncodeJson(document);
            if (!validated.success) {
                error = validated.error;
                return false;
            }
            const query::DocumentView view(std::string_view(validated.value).substr(DocumentCodec::kEnvelopeSize));
            const auto identity = view.Get(query::CompiledFieldPath("_id"));
            if (identity.found() && (identity.type != query::DocumentValueType::String ||
                                    identity.string_value.empty() || identity.string_value.size() > 1024)) {
                error = "Document _id must be a nonempty string of at most 1 KiB";
                return false;
            }
            if (identity.found() && std::none_of(schema.fields.begin(), schema.fields.end(),
                            [](const SchemaField& field) { return field.default_val.has_value(); })) {
                if (encoded) *encoded = std::move(validated.value);
                return true;
            }
            auto parsed = nlohmann::ordered_json::parse(document, nullptr, false);
            if (!parsed.is_object()) {
                error = "Default input must be a JSON object";
                return false;
            }
            for (const auto& field : schema.fields) {
                if (!field.default_val) continue;
                const query::CompiledFieldPath path(field.name);
                if (!path.valid()) { error = path.error(); return false; }
                auto* parent = &parsed;
                const auto& components = path.components();
                for (std::size_t i = 0; i + 1 < components.size(); ++i) {
                    if (!parent->contains(components[i]))
                        (*parent)[components[i]] = nlohmann::ordered_json::object();
                    parent = &(*parent)[components[i]];
                    if (!parent->is_object()) {
                        error = "Default path traverses a non-object: " + field.name;
                        return false;
                    }
                }
                if (parent->contains(components.back())) continue; // Null is not missing.
                auto value = nlohmann::ordered_json::parse(*field.default_val, nullptr, false);
                if (value.is_discarded()) {
                    if (field.type != FieldType::String) {
                        error = "Invalid JSON schema default: " + field.name;
                        return false;
                    }
                    value = *field.default_val; // Legacy native plain-string default.
                }
                (*parent)[components.back()] = std::move(value);
            }
            if (!parsed.contains("_id")) parsed["_id"] = GenerateDocId();
            if (!parsed["_id"].is_string() || parsed["_id"].get_ref<const std::string&>().empty()) {
                error = "Default _id must be a nonempty string";
                return false;
            }
            document = parsed.dump();
            if (encoded) {
                auto result = DocumentCodec::EncodeJson(document);
                if (!result.success) { error = result.error; return false; }
                *encoded = std::move(result.value);
            }
            return true;
        }

        bool DocEngine::ValidateDocument(const std::string&      bson_document,
                                         const SchemaDefinition& schema,
                                         std::string&            error_out) const {
            auto encoded_document = DocumentCodec::EncodeJson(bson_document);
            if (!encoded_document.success) {
                error_out = encoded_document.error;
                return false;
            }
            const query::DocumentView document_view(
                    std::string_view(encoded_document.value).substr(
                            DocumentCodec::kEnvelopeSize));
            if (!document_view.valid()) {
                error_out = "invalid BSON document";
                return false;
            }
            for (const auto& field : schema.fields) {
                const query::CompiledFieldPath path(field.name);
                if (!path.valid()) {
                    error_out = path.error();
                    return false;
                }
                const auto value = document_view.Get(path);
                if (field.required && !value.found()) {
                    error_out = "Required field '" + field.name + "' is missing";
                    return false;
                }
                if (!value.found()) continue;
                using Type = query::DocumentValueType;
                bool compatible = false;
                switch (field.type) {
                    case FieldType::String: compatible = value.type == Type::String; break;
                    case FieldType::Int32:
                        compatible = value.type == Type::Int64 &&
                                     value.int_value >= std::numeric_limits<std::int32_t>::min() &&
                                     value.int_value <= std::numeric_limits<std::int32_t>::max();
                        break;
                    case FieldType::Int64: compatible = value.type == Type::Int64; break;
                    case FieldType::Float64: compatible = value.type == Type::Float64; break;
                    case FieldType::Bool: compatible = value.type == Type::Bool; break;
                    case FieldType::Array: compatible = value.type == Type::Array; break;
                    case FieldType::Object: compatible = value.type == Type::Document; break;
                    case FieldType::Binary: compatible = value.type == Type::Binary; break;
                    case FieldType::Null: compatible = value.type == Type::Null; break;
                }
                if (!compatible) {
                    error_out = "Schema type mismatch for field '" + field.name + "'";
                    return false;
                }
            }
            if (schema.strict) {
                bson_t document;
                const auto payload = std::string_view(encoded_document.value).substr(DocumentCodec::kEnvelopeSize);
                if (!bson_init_static(&document, reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size())) {
                    error_out = "Invalid BSON schema input";
                    return false;
                }
                bson_iter_t iterator;
                bson_iter_init(&iterator, &document);
                while (bson_iter_next(&iterator)) {
                    const std::string_view key(bson_iter_key(&iterator));
                    if (key == "_id") continue;
                    const bool known = std::any_of(schema.fields.begin(), schema.fields.end(),
                            [&](const SchemaField& field) {
                                return key == std::string_view(field.name).substr(0, field.name.find('.'));
                            });
                    if (!known) {
                        error_out = "Unknown field in strict schema: " + std::string(key);
                        return false;
                    }
                }
            }
            return true;
        }

// ══════════════════════════════════════════════════════════════
// §9  CRUD - Insert
// ══════════════════════════════════════════════════════════════

        DBResult DocEngine::InsertOneImpl(
                const std::string& collection_name,
                const std::string& input_document) {
            std::string bson_document = input_document;
            TouchUserActivity();
            std::shared_lock<std::shared_mutex> ddl_lock(index_ddl_mutex_,std::defer_lock);
            {WriteStageTimer timer(operations_,OperationsState::DdlWaitStage);ddl_lock.lock();}
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

            std::string default_error;
            if (!ApplyInsertDefaults(bson_document, metadata.schema, default_error))
                return DBResult::Err(default_error, "validation_failed");

            if (metadata.schema.strict || !metadata.schema.fields.empty()) {
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
            if (document_id.size() > 1024) {
                return DBResult::Err("Document _id exceeds the 1 KiB limit");
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

        DBResult DocEngine::InsertManyImpl(const std::string&              collection_name,
                                       const std::vector<std::string_view>& bson_documents,
                                       bool bson_input) {
            TouchUserActivity();
            std::shared_lock<std::shared_mutex> ddl_lock(index_ddl_mutex_,std::defer_lock);
            {WriteStageTimer timer(operations_,OperationsState::DdlWaitStage);ddl_lock.lock();}
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

            for (const auto input : bson_documents) {
                std::string bson;
                if (!bson_input) bson.assign(input);
                std::string default_error;
                std::string encoded_document;
                bool prepared = false;
                if (bson_input) {
                    auto encoded = DocumentCodec::EncodeBson(input);
                    if (!encoded.success) return DBResult::Err(encoded.error, "validation_failed");
                    encoded_document = std::move(encoded.value);
                    const query::DocumentView view(std::string_view(encoded_document).substr(
                            DocumentCodec::kEnvelopeSize));
                    static const query::CompiledFieldPath bson_identity_path("_id");
                    const auto identity = view.Get(bson_identity_path);
                    if (identity.found() && (identity.type != query::DocumentValueType::String ||
                            identity.string_value.empty() || identity.string_value.size() > 1024))
                        return DBResult::Err("Document _id must be a nonempty string of at most 1 KiB", "validation_failed");
                    prepared = identity.found() && !metadata.schema.strict &&
                            metadata.schema.fields.empty() && metadata.foreign_keys.empty();
                    if (!prepared) {
                        auto decoded = DocumentCodec::DecodeToJson(encoded_document);
                        if (!decoded.success) return DBResult::Err(decoded.error, "validation_failed");
                        bson = std::move(decoded.value);
                    }
                }
                if (!prepared && !ApplyInsertDefaults(bson, metadata.schema, default_error, &encoded_document))
                    return DBResult::Err(default_error, "validation_failed");
                if (metadata.schema.strict || !metadata.schema.fields.empty()) {
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
                status = metadata.foreign_keys.empty() ? rocksdb::Status::OK() :
                        ValidateForeignKeysChecked(metadata.foreign_keys, bson, validation_error);
                if (!status.ok()) {
                    if (!validation_error.empty()) {
                        return DBResult::Err(validation_error);
                    }
                    return RocksFailure(
                            "InsertMany foreign-key validation",
                            status);
                }

                const query::DocumentView document_view(std::string_view(encoded_document).substr(
                        DocumentCodec::kEnvelopeSize));
                static const query::CompiledFieldPath identity_path("_id");
                std::string doc_id = ScalarToString(document_view.Get(identity_path));
                if (doc_id.empty()) {
                    doc_id = GenerateDocId();
                }
                if (doc_id.size() > 1024) {
                    return DBResult::Err(
                            "Document _id exceeds the 1 KiB limit");
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

                status = mutation.PutEncodedDocument(doc_id, encoded_document);

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
            return DBResult::Ok(nlohmann::json(created_ids).dump());
        }

        BulkWriteResult DocEngine::InsertManyBulk(
                const std::string& collection_name,
                const std::vector<std::string>& bson_documents,
                const BulkWriteOptions& options) {
            TouchUserActivity();
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

        DBResult DocEngine::FindByIdImpl(const std::string& collection_name,
                                     const std::string& doc_id) {
            TouchUserActivity();
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");
            if (!CollectionExists(collection_name))
                return DBResult::Err("Collection '" + collection_name + "' does not exist");
            std::string bson;
            auto read_options=read_options_;
            read_options.fill_cache=(query_tuning_.load(std::memory_order_relaxed)&1)!=0;
            rocksdb::Status s = txn_db_->Get(read_options,
                                             MakeDocKey(collection_name, doc_id),
                                             &bson);
            if (s.IsNotFound())
                return DBResult::Err("Document '" + doc_id + "' not found in '" +
                                     collection_name + "'", "not_found");
            ROCKS_CHECK(s, "FindById");
            if(bson.size()>(SIZE_MAX-4096)/8) throw ResourceExhausted("point read document size overflow");
            BudgetLease decode_credit(ExecutionBudget(),ResourceBudget::Buffer,bson.size()*8+4096);
            std::string json;
            s = DecodeStoredDocument(bson, json);
            if (!s.ok()) return RocksFailure("FindById decode", s);
            auto credit=std::make_shared<BudgetLease>(ExecutionBudget(),ResourceBudget::Query,json.capacity());
            auto result=DBResult::Ok(std::move(json));result.memory_lease=std::move(credit);return result;
        }

        DBResult DocEngine::FindMany(const std::string&              collection_name,
                                     const nexora::query::Condition& condition,
                                     uint32_t                        limit,
                                     uint32_t                        skip) {
            return ExecuteQuery(collection_name, condition, limit, skip, false).result;
        }

        DBResult DocEngine::FindManyFullScanForTesting(
                const std::string& collection_name,
                const nexora::query::Condition& condition,
                std::uint32_t limit,
                std::uint32_t skip) {
            return ExecuteQuery(collection_name, condition, limit, skip, true).result;
        }

        DocEngine::QueryExecution DocEngine::ExecuteQueryImpl(
                const std::string& collection_name,
                const nexora::query::Condition& condition,
                std::uint32_t limit,
                std::uint32_t skip,
                bool force_full_scan,const rocksdb::Snapshot* shared_epoch,bool ddl_already_locked,bool count_only) {
            std::shared_lock ddl_lock(index_ddl_mutex_,std::defer_lock);
            if (!ddl_already_locked) ddl_lock.lock();
            ReadEpoch epoch(txn_db_.get(),shared_epoch);
            auto read_epoch=read_options_; read_epoch.snapshot=epoch.get();
            const auto tuning=query_tuning_.load(std::memory_order_relaxed);
            read_epoch.fill_cache=(tuning&1)!=0;
            force_full_scan=force_full_scan || !(tuning&2);
            QueryExecution execution;
            TouchUserActivity();
            if (IsReservedCollectionName(collection_name))
                { execution.result = DBResult::Err("reserved collection name"); return execution; }
            if (!CollectionExists(collection_name))
                { execution.result = DBResult::Err("Collection '" + collection_name + "' does not exist"); return execution; }

            auto result_credit=std::make_shared<BudgetLease>(ExecutionBudget(),ResourceBudget::Query);
            std::string results_json;
            const std::size_t expected_results = limit > 0 ? limit : 16;
            const auto initial=std::min<std::size_t>(expected_results,4096)*256+2;
            result_credit->resize(initial);
            results_json.reserve(initial);
            results_json.push_back('[');
            bool first = true;
            std::uint32_t count = 0;
            std::uint32_t skipped = 0;
            const nexora::query::CompiledQuery compiled_query(condition);
            if (!compiled_query.valid()) {
                execution.result = DBResult::Err(
                        "Invalid query: " + compiled_query.error());
                return execution;
            }
            rocksdb::Status decode_status = rocksdb::Status::OK();
            auto append_if_match = [&](const std::string& stored_document) {
                CheckExecution();
                if(stored_document.size()>(SIZE_MAX-4096)/8) throw ResourceExhausted("query document size overflow");
                BudgetLease scratch(ExecutionBudget(),ResourceBudget::Buffer,stored_document.size()*8+4096);
                std::string converted_document;
                std::string_view bson_payload;
                switch (DocumentCodec::Detect(stored_document)) {
                    case DocumentCodec::Format::BsonV1:
                        bson_payload = std::string_view(stored_document).substr(
                                DocumentCodec::kEnvelopeSize);
                        break;
                    case DocumentCodec::Format::LegacyJson: {
                        auto encoded = DocumentCodec::EncodeJson(stored_document);
                        if (!encoded.success) {
                            decode_status = rocksdb::Status::Corruption(
                                    "legacy document conversion failed: " +
                                    encoded.error);
                            return true;
                        }
                        converted_document = std::move(encoded.value);
                        bson_payload = std::string_view(converted_document).substr(
                                DocumentCodec::kEnvelopeSize);
                        break;
                    }
                    case DocumentCodec::Format::UnknownEnvelope:
                        decode_status = rocksdb::Status::Corruption(
                                "unsupported document envelope");
                        return true;
                }
                const nexora::query::DocumentView document_view(bson_payload);
                if (!document_view.valid()) {
                    decode_status = rocksdb::Status::Corruption(
                            "invalid BSON document view");
                    return true;
                }
                ++execution.scanned_documents;
                try {
                    if (!compiled_query.Match(document_view)) return false;
                } catch (const std::exception& exception) {
                    decode_status = rocksdb::Status::Aborted("Query evaluation failed", exception.what());
                    return true;
                }
                ++execution.matched_documents;
                if (skipped < skip) { ++skipped; return false; }
                std::string document;
                decode_status = DecodeStoredDocument(
                        stored_document, document);
                if (!decode_status.ok()) return true;
                if(count_only) return false;
                const auto required=results_json.size()+document.size()+2;
                if(required>results_json.capacity()) {
                    const auto target=std::max(required,results_json.capacity()*2);
                    result_credit->resize(target+results_json.capacity()); // old + replacement during growth
                    results_json.reserve(target);
                    result_credit->resize(results_json.capacity());
                }
                if (!first) results_json.push_back(',');
                first = false;
                results_json += document;
                ++count;
                return limit > 0 && count >= limit;
            };

            const auto indexes = GetIndexes(collection_name);
            const SelectedIndexPlan plan = force_full_scan
                    ? SelectedIndexPlan{} : SelectIndexPlan(indexes, condition);

            if (plan.index) {
                execution.index_name = plan.index->index_name;
                execution.scan_type = plan.range ? "index_range" : "index_equality";
                std::vector<query::FieldValue> equality_values;
                for (const auto* equality : plan.equalities)
                    equality_values.push_back({equality->value,
                                               equality->value_type, true});
                const auto encoded_equalities = indexv2::EncodeValues(equality_values);
                if (!encoded_equalities) {
                    execution.result = DBResult::Err("Unable to encode index predicate");
                    return execution;
                }
                std::string prefix = indexv2::IndexPrefix(
                        collection_name, plan.index->index_id);
                prefix.push_back('E');
                prefix.append(*encoded_equalities);
                std::string seek_key = prefix;
                std::string boundary;
                if (plan.range) {
                    const auto encoded_range = indexv2::EncodeValues({
                            query::FieldValue{plan.range->value,
                                              plan.range->value_type, true}});
                    if (!encoded_range) {
                        execution.result = DBResult::Err("Unable to encode range predicate");
                        return execution;
                    }
                    boundary = prefix + *encoded_range;
                    if (plan.range->op == query::Op::GT ||
                        plan.range->op == query::Op::GTE)
                        seek_key = boundary;
                }
                const std::string upper_value = PrefixUpperBound(prefix);
                const rocksdb::Slice upper_bound(upper_value);
                rocksdb::ReadOptions options=MakeScanReadOptions(read_epoch);
                options.iterate_upper_bound = &upper_bound;
                if (db_options_.prefix_extractor && db_options_.prefix_extractor->InDomain(seek_key)) {
                    options.total_order_seek=false; options.prefix_same_as_start=true;
                }
                auto iterator = std::unique_ptr<rocksdb::Iterator>(
                        txn_db_->NewIterator(options));
                iterator->Seek(seek_key);
                bool finished=false;
                while (iterator->Valid() && !finished) {
                    CheckExecution();
                    std::vector<std::string> keys;
                    keys.reserve(64);
                    while (iterator->Valid() && keys.size()<64) {
                        CheckExecution();
                    const std::string physical_key = iterator->key().ToString();
                    if (plan.range) {
                        if (plan.range->op == query::Op::GT &&
                            physical_key.starts_with(boundary)) { iterator->Next(); continue; }
                        if (plan.range->op == query::Op::LT &&
                            physical_key >= boundary) { finished=true; break; }
                        if (plan.range->op == query::Op::LTE &&
                            physical_key > boundary &&
                            !physical_key.starts_with(boundary)) { finished=true; break; }
                    }
                    ++execution.scanned_index_entries;
                    keys.push_back(MakeDocKey(collection_name,iterator->value().ToString()));
                    iterator->Next();
                    }
                    std::vector<rocksdb::Slice> slices; slices.reserve(keys.size());
                    for (const auto& key:keys) slices.emplace_back(key);
                    std::vector<std::string> documents;
                    if (!keys.empty()) {
                        ++multiget_calls_; multiget_keys_+=keys.size();
                        const auto statuses=txn_db_->MultiGet(read_epoch,slices,&documents);
                        for (size_t index=0;index<statuses.size();++index) {
                            if (statuses[index].IsNotFound()) {
                                execution.result = RocksFailure("indexed query dangling entry",
                                        rocksdb::Status::Corruption("index references missing document"));
                                return execution;
                            }
                            if (!statuses[index].ok()) {
                                execution.result=RocksFailure("indexed query MultiGet",statuses[index]); return execution;
                            }
                            if (append_if_match(documents[index])) { finished=true; break; }
                        }
                    }
                }
                if (!decode_status.ok()) {
                    execution.result = RocksFailure(
                            "indexed query document decode", decode_status);
                    return execution;
                }
                if (!iterator->status().ok()) {
                    execution.result = RocksFailure("index scan", iterator->status());
                    return execution;
                }
            } else {
                const std::string data_prefix = std::string(keys::kData) + collection_name + ":";
                const std::string upper_value = PrefixUpperBound(data_prefix);
                const rocksdb::Slice upper_bound(upper_value);
                rocksdb::ReadOptions options=MakeScanReadOptions(read_epoch);
                options.fill_cache = false;
                options.iterate_upper_bound = &upper_bound;
                auto iterator = std::unique_ptr<rocksdb::Iterator>(
                        txn_db_->NewIterator(options));
                for (iterator->Seek(data_prefix); iterator->Valid(); iterator->Next())
                    if (append_if_match(iterator->value().ToString())) break;
                if (!decode_status.ok()) {
                    execution.result = RocksFailure(
                            "full scan document decode", decode_status);
                    return execution;
                }
                if (!iterator->status().ok()) {
                    execution.result = RocksFailure("full scan", iterator->status());
                    return execution;
                }
            }

            results_json.push_back(']');
            execution.result = DBResult::Ok(std::move(results_json));
            execution.result.memory_lease=std::move(result_credit);
            return execution;
        }

        DBResult DocEngine::ExplainPlan(
                const std::string& collection_name,
                const nexora::query::Condition& condition,
                bool force_full_scan) {
            const QueryExecution execution = ExecuteQuery(
                    collection_name, condition, 0, 0, force_full_scan);
            if (!execution.result.success) return execution.result;
            return DBResult::Ok(
                    "{\"scan_type\":\"" + execution.scan_type +
                    "\",\"index\":\"" + execution.index_name +
                    "\",\"scanned_index_entries\":" +
                    std::to_string(execution.scanned_index_entries) +
                    ",\"scanned_documents\":" +
                    std::to_string(execution.scanned_documents) + "}");
        }

        PageResult DocEngine::FindPage(
                const std::string& collection_name,
                const nexora::query::Condition& condition,
                std::uint32_t limit,
                const std::string& continuation_token,size_t max_bytes) {
            PageResult result;
            if (limit == 0) {
                result.error_msg = "Page limit must be greater than zero";
                return result;
            }
            if(limit>pagination::kMaxRecords || max_bytes<pagination::kMinBytes || max_bytes>pagination::kMaxBytes) {
                result.error_msg="Page exceeds bounded limit (1000 records / 64MiB)";
                return result;
            }
            std::shared_ptr<BudgetLease> page_credit;
            try {page_credit=std::make_shared<BudgetLease>(ExecutionBudget(),ResourceBudget::Buffer,max_bytes*2);}
            catch(const ResourceExhausted& error) {result.error_msg=error.what();return result;}
            TouchUserActivity();
            if (!CollectionExists(collection_name)) {
                result.error_msg = "Collection '" + collection_name + "' does not exist";
                return result;
            }
            std::unique_lock<std::mutex> sessions_lock(page_sessions_mutex_);
            PrunePageSessionsLocked();
            const auto now = PageSessionNow();

            std::string session_id;
            std::string cursor_key;
            std::shared_ptr<PageSession> session;
            if (continuation_token.empty()) {
                // Native callers bypass HTTP admission. Bound retained
                // snapshots here too, before allocating another session.
                if (page_live_snapshots_->load() >= pagination::kMaxSnapshots) {
                    result.error_msg = "Page snapshot capacity exhausted; close existing cursors";
                    return result;
                }
                session_id = GenerateDocId();
                session = std::make_shared<PageSession>();
                try {session->memory_credit=std::make_shared<BudgetLease>(ExecutionBudget(),ResourceBudget::Snapshot,
                    sizeof(PageSession)+collection_name.size()+8192);}
                catch(const ResourceExhausted& error) {result.error_msg=error.what();return result;}
                const auto* snapshot = txn_db_->GetSnapshot();
                if (!snapshot) {
                    result.error_msg = "Unable to acquire page snapshot";
                    return result;
                }
                page_live_snapshots_->fetch_add(1);
                session->snapshot = std::shared_ptr<const rocksdb::Snapshot>(snapshot,
                    [db=txn_db_.get(), live=page_live_snapshots_](const rocksdb::Snapshot* value) {
                        db->ReleaseSnapshot(value);
                        live->fetch_sub(1);
                    });
                session->collection = collection_name;
                session->condition = condition;
                session->created_at = now;
                session->expires_at = now + std::chrono::seconds(pagination::kIdleLeaseSeconds);
                page_sessions_.emplace(session_id, session);
            } else {
                if (!continuation_token.starts_with("s1.")) {
                    result.error_msg = "Invalid continuation token";
                    return result;
                }
                const std::size_t separator = continuation_token.find('.', 3);
                if (separator == std::string::npos) {
                    result.error_msg = "Invalid continuation token";
                    return result;
                }
                session_id = continuation_token.substr(3, separator - 3);
                const auto decoded = DecodeCursorToken(
                        continuation_token.substr(separator + 1));
                const auto found = page_sessions_.find(session_id);
                if (!decoded || found == page_sessions_.end() ||
                    found->second->collection != collection_name || found->second->expires_at<=now) {
                    result.error_msg = "Expired or invalid continuation token";
                    return result;
                }
                cursor_key = *decoded;
                session = found->second;
                // A progressing scan keeps its original snapshot. Expire idle
                // cursors rather than truncating valid full scans at five minutes.
                session->expires_at = now + std::chrono::seconds(pagination::kIdleLeaseSeconds);
            }
            // Registry lock protects membership/expiry only. Independent
            // cursors scan concurrently; close/expiry cannot free an active
            // snapshot because this call pins the shared session.
            sessions_lock.unlock();
            std::lock_guard scan_lock(session->scan_mutex);
            const auto discard_session = [&] {
                std::lock_guard registry_lock(page_sessions_mutex_);
                const auto found=page_sessions_.find(session_id);
                if(found!=page_sessions_.end() && found->second==session) page_sessions_.erase(found);
            };
            struct SessionFailureGuard {
                const decltype(discard_session)& retire;
                PageResult& result;
                ~SessionFailureGuard() {if(!result.success) retire();}
            } failure_guard{discard_session,result};
            const nexora::query::CompiledQuery compiled_query(
                    session->condition);
            if (!compiled_query.valid()) {
                result.error_msg = "Invalid query: " + compiled_query.error();
                discard_session();
                return result;
            }
            const std::string prefix = std::string(keys::kData) + collection_name + ":";
            const std::string upper_value = PrefixUpperBound(prefix);
            const rocksdb::Slice upper_bound(upper_value);
            rocksdb::ReadOptions options=MakeScanReadOptions(read_options_);
            options.fill_cache = false;
            options.iterate_upper_bound = &upper_bound;
            options.snapshot = session->snapshot.get();
            auto iterator = std::unique_ptr<rocksdb::Iterator>(
                    txn_db_->NewIterator(options));

            if (cursor_key.empty()) {
                iterator->Seek(prefix);
            } else {
                if (!cursor_key.starts_with(prefix)) {
                    result.error_msg = "Invalid continuation token";
                    return result;
                }
                iterator->Seek(cursor_key);
                if (iterator->Valid() && iterator->key() == cursor_key)
                    iterator->Next();
            }

            std::string json;
            const auto reserve_bytes=static_cast<std::size_t>(limit)*256+2;
            json.reserve(max_bytes ? std::min(reserve_bytes,max_bytes) : reserve_bytes);
            json.push_back('[');
            bool first = true;
            std::uint32_t emitted = 0;
            std::string last_key;
            for (; iterator->Valid() && emitted < limit; iterator->Next()) {
                try { CheckExecution(); } catch(...) {discard_session();throw;}
                // Borrow only until iterator->Next(). Never expose this view
                // to Python or retain it in a page/session.
                const auto value = iterator->value();
                const std::string_view stored_document(value.data(), value.size());
                bool matched = false;
                const rocksdb::Status match_status = MatchDocument(
                        stored_document, compiled_query, matched);
                if (!match_status.ok()) {
                    result.error_msg = match_status.ToString();
                    discard_session();
                    return result;
                }
                if (!matched) continue;
                std::unique_ptr<BudgetLease> decode_credit;
                try {
                    if(stored_document.size()>(SIZE_MAX-4096)/8) throw ResourceExhausted("page document size overflow");
                    decode_credit=std::make_unique<BudgetLease>(ExecutionBudget(),ResourceBudget::Buffer,stored_document.size()*8+4096);
                } catch(const ResourceExhausted& error) {result.error_msg=error.what();discard_session();return result;}
                std::string document;
                const rocksdb::Status decode_status = DecodeStoredDocument(
                        stored_document, document);
                if (!decode_status.ok()) {
                    result.error_msg = decode_status.ToString();
                    discard_session();
                    return result;
                }
                if (max_bytes && document.size()+2>max_bytes) {
                    result.error_msg="document exceeds page byte budget";
                    discard_session();
                    return result;
                }
                if (max_bytes && json.size()+document.size()+(first ? 0 : 1)+1>max_bytes) break;
                if (!first) json.push_back(',');
                first = false;
                json += document;
                last_key = iterator->key().ToString();
                ++emitted;
            }
            if (!iterator->status().ok()) {
                result.error_msg = iterator->status().ToString();
                discard_session();
                return result;
            }
            json.push_back(']');
            result.success = true;
            result.data = std::move(json);
            page_credit->resize(result.data.capacity());
            result.memory_lease=std::move(page_credit);
            if (!last_key.empty() && iterator->Valid()) {
                // Idle time starts after successful work, not before a slow page.
                // ClosePage/PrepareShutdown must never be undone by this renewal.
                std::lock_guard registry_lock(page_sessions_mutex_);
                const auto found = page_sessions_.find(session_id);
                if (found==page_sessions_.end() || found->second!=session) {
                    result.success = false;
                    result.data = "[]";
                    result.error_msg = "Page session closed during scan";
                    return result;
                }
                session->expires_at = PageSessionNow() + std::chrono::seconds(pagination::kIdleLeaseSeconds);
                result.continuation_token = "s1." + session_id + "." +
                        EncodeCursorToken(last_key);
            } else {
                discard_session();
            }
            return result;
        }

// ══════════════════════════════════════════════════════════════
// §11  CRUD - Update
// ══════════════════════════════════════════════════════════════

        void DocEngine::ClosePage(const std::string& token) {
            if (!token.starts_with("s1.")) return;
            const auto end=token.find('.',3); if (end==std::string::npos) return;
            std::lock_guard lock(page_sessions_mutex_);
            const auto found=page_sessions_.find(token.substr(3,end-3));
            if (found!=page_sessions_.end()) {
                page_sessions_.erase(found);
            }
        }

        DBResult DocEngine::UpdateByIdImpl(
                const std::string& collection_name,
                const std::string& doc_id,
                const nexora::query::UpdateSpec& update_spec) {
            TouchUserActivity();
            std::shared_lock<std::shared_mutex> ddl_lock(index_ddl_mutex_);
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

            std::string new_document;
            status = ApplyUpdate(
                    old_document, update_spec, new_document);
            if (!status.ok())
                return RocksFailure("UpdateById apply update", status);

            if (metadata.schema.strict || !metadata.schema.fields.empty()) {
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
                                       const nexora::query::UpdateSpec& update_spec) try {
            TouchUserActivity();
            std::shared_lock<std::shared_mutex> ddl_lock(index_ddl_mutex_);
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
            const nexora::query::CompiledQuery compiled_query(condition);
            if (!compiled_query.valid()) {
                return DBResult::Err("Invalid query: " + compiled_query.error());
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
            BudgetLease candidate_credit(ExecutionBudget(),ResourceBudget::Buffer);
            std::vector<std::string> candidate_ids;

            {
                rocksdb::ReadOptions scan_options = MakeScanReadOptions(read_options_);
                scan_options.fill_cache = false;
                const std::string upper_value = PrefixUpperBound(data_prefix);
                const rocksdb::Slice upper_bound(upper_value);
                scan_options.iterate_upper_bound = &upper_bound;
                auto iterator = mutation.NewIterator(scan_options);
                for (iterator->Seek(data_prefix);
                     iterator->Valid() &&
                     iterator->key().starts_with(data_prefix);
                     iterator->Next()) {
                    bool matched = false;
                    status = MatchDocument(
                            iterator->value().ToString(), compiled_query, matched);
                    if (!status.ok()) {
                        return RocksFailure(
                                "UpdateMany document match", status);
                    }
                    if (matched) {
                        candidate_credit.resize(candidate_credit.bytes()+4*(iterator->key().size()+sizeof(std::string)));
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
                bool still_matches = false;
                status = MatchDocument(
                        old_document, compiled_query, still_matches);
                if (!status.ok()) {
                    return RocksFailure("UpdateMany document recheck", status);
                }
                if (!still_matches) {
                    continue;
                }

                std::string new_document;
                status = ApplyUpdate(
                        old_document, update_spec, new_document);
                if (!status.ok())
                    return RocksFailure("UpdateMany apply update", status);

                if (metadata.schema.strict || !metadata.schema.fields.empty()) {
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
        } catch(const ResourceExhausted& error) {return DBResult::Err(error.what(),"resource_exhausted");}

        BulkWriteResult DocEngine::UpdateManyBulk(
                const std::string& collection_name,
                const nexora::query::Condition& condition,
                const nexora::query::UpdateSpec& update_spec,
                const BulkWriteOptions& options) {
            TouchUserActivity();
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
            const nexora::query::CompiledQuery compiled_query(condition);
            if (!compiled_query.valid()) {
                result.last_error = "Invalid query: " + compiled_query.error();
                return result;
            }

            const std::string prefix =
                    std::string(keys::kData) + collection_name + ":";
            std::vector<std::pair<std::string, std::size_t>> candidates;
            rocksdb::ReadOptions scan_options = MakeScanReadOptions(read_options_);
            scan_options.fill_cache = false;
            const std::string upper_value = PrefixUpperBound(prefix);
            const rocksdb::Slice upper_bound(upper_value);
            scan_options.iterate_upper_bound = &upper_bound;
            auto iterator = std::unique_ptr<rocksdb::Iterator>(
                    txn_db_->NewIterator(scan_options));
            for (iterator->Seek(prefix);
                 iterator->Valid() && iterator->key().starts_with(prefix);
                 iterator->Next()) {
                const std::string stored_document = iterator->value().ToString();
                bool matched = false;
                const rocksdb::Status match_status = MatchDocument(
                        stored_document, compiled_query, matched);
                if (!match_status.ok()) {
                    result.last_error = RocksFailure(
                            "UpdateManyBulk document match",
                            match_status).error_msg;
                    return result;
                }
                if (matched) {
                    candidates.emplace_back(
                            iterator->key().ToString().substr(prefix.size()),
                            iterator->key().size() + stored_document.size());
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

        DBResult DocEngine::DeleteByIdImpl(
                const std::string& collection_name,
                const std::string& doc_id) {
            TouchUserActivity();
            std::shared_lock<std::shared_mutex> ddl_lock(index_ddl_mutex_);
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
                                       const nexora::query::Condition& condition) try {
            TouchUserActivity();
            std::shared_lock<std::shared_mutex> ddl_lock(index_ddl_mutex_);
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
            const nexora::query::CompiledQuery compiled_query(condition);
            if (!compiled_query.valid()) {
                return DBResult::Err("Invalid query: " + compiled_query.error());
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
            BudgetLease candidate_credit(ExecutionBudget(),ResourceBudget::Buffer);
            std::vector<std::string> candidate_ids;

            {
                rocksdb::ReadOptions scan_options = MakeScanReadOptions(read_options_);
                scan_options.fill_cache = false;
                const std::string upper_value = PrefixUpperBound(data_prefix);
                const rocksdb::Slice upper_bound(upper_value);
                scan_options.iterate_upper_bound = &upper_bound;
                auto iterator = mutation.NewIterator(scan_options);
                for (iterator->Seek(data_prefix);
                     iterator->Valid() &&
                     iterator->key().starts_with(data_prefix);
                     iterator->Next()) {
                    bool matched = false;
                    status = MatchDocument(
                            iterator->value().ToString(), compiled_query, matched);
                    if (!status.ok()) {
                        return RocksFailure(
                                "DeleteMany document match", status);
                    }
                    if (matched) {
                        candidate_credit.resize(candidate_credit.bytes()+4*(iterator->key().size()+sizeof(std::string)));
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
                bool still_matches = false;
                status = MatchDocument(
                        old_document, compiled_query, still_matches);
                if (!status.ok()) {
                    return RocksFailure("DeleteMany document recheck", status);
                }
                if (!still_matches) {
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
        } catch(const ResourceExhausted& error) {return DBResult::Err(error.what(),"resource_exhausted");}

        BulkWriteResult DocEngine::DeleteManyBulk(
                const std::string& collection_name,
                const nexora::query::Condition& condition,
                const BulkWriteOptions& options) {
            TouchUserActivity();
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
            const nexora::query::CompiledQuery compiled_query(condition);
            if (!compiled_query.valid()) {
                result.last_error = "Invalid query: " + compiled_query.error();
                return result;
            }

            const std::string prefix =
                    std::string(keys::kData) + collection_name + ":";
            std::vector<std::pair<std::string, std::size_t>> candidates;
            rocksdb::ReadOptions scan_options = MakeScanReadOptions(read_options_);
            scan_options.fill_cache = false;
            const std::string upper_value = PrefixUpperBound(prefix);
            const rocksdb::Slice upper_bound(upper_value);
            scan_options.iterate_upper_bound = &upper_bound;
            auto iterator = std::unique_ptr<rocksdb::Iterator>(
                    txn_db_->NewIterator(scan_options));
            for (iterator->Seek(prefix);
                 iterator->Valid() && iterator->key().starts_with(prefix);
                 iterator->Next()) {
                const std::string stored_document = iterator->value().ToString();
                bool matched = false;
                const rocksdb::Status match_status = MatchDocument(
                        stored_document, compiled_query, matched);
                if (!match_status.ok()) {
                    result.last_error = RocksFailure(
                            "DeleteManyBulk document match",
                            match_status).error_msg;
                    return result;
                }
                if (matched) {
                    candidates.emplace_back(
                            iterator->key().ToString().substr(prefix.size()),
                            iterator->key().size() + stored_document.size());
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
                throw std::runtime_error("IterateCollection: reserved collection name");
            }
            if (!CollectionExists(collection_name)) {
                throw std::runtime_error("IterateCollection: collection not found: " + collection_name);
            }

            std::string data_prefix = std::string(keys::kData) + collection_name + ":";

            // fill_cache=false مناسب bulk scan — مصرف cache RocksDB را کاهش می‌دهد
            rocksdb::ReadOptions ro=MakeScanReadOptions(read_options_);
            ro.fill_cache       = false;
            ro.total_order_seek = true;
            const std::string upper_value = PrefixUpperBound(data_prefix);
            const rocksdb::Slice upper_bound(upper_value);
            ro.iterate_upper_bound = &upper_bound;

            auto it = std::unique_ptr<rocksdb::Iterator>(txn_db_->NewIterator(ro));

            for (it->Seek(data_prefix);
                 it->Valid() && it->key().starts_with(data_prefix); it->Next()) {

                std::string doc_id    = it->key().ToString().substr(data_prefix.size());
                std::string bson_data;
                const rocksdb::Status decode_status = DecodeStoredDocument(
                        it->value().ToString(), bson_data);
                if (!decode_status.ok()) {
                    throw std::runtime_error("IterateCollection decode: " + decode_status.ToString());
                }

                if (!callback(doc_id, bson_data)) break;  // early exit
            }
            if (!it->status().ok())
                throw std::runtime_error("IterateCollection scan: " + it->status().ToString());

            // batch_size برای آینده (yield/throttle) نگه داشته شده
            (void)batch_size;
        }

        int64_t DocEngine::GetCollectionSize(const std::string& collection_name) const {
            if (IsReservedCollectionName(collection_name)) return -1;
            std::string seq_key = std::string(keys::kSeq) + collection_name;
            std::string val;
            rocksdb::Status s = txn_db_->Get(read_options_, seq_key, &val);
            if (!s.ok() || val.empty()) return -1;
            try {
                std::size_t parsed = 0;
                const auto size = std::stoll(val, &parsed);
                return parsed == val.size() && size >= 0 &&
                    val.find_first_not_of("0123456789") == std::string::npos ? size : -1;
            } catch (...) { return -1; }
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
                    transaction->GetIterator(MakeScanReadOptions(read_options_)));
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

            rocksdb::ReadOptions ro=MakeScanReadOptions(read_options_);
            ro.fill_cache = false;
            auto it = std::unique_ptr<rocksdb::Iterator>(txn_db_->NewIterator(ro));

            for (it->Seek(seek_key);
                 it->Valid() && it->key().starts_with(scan_prefix); it->Next()) {
                std::string doc_id = it->key().ToString().substr(scan_prefix.size());
                if (!id_prefix.empty() && doc_id.find(id_prefix) != 0) break;
                std::string json_document;
                if (!DecodeStoredDocument(
                            it->value().ToString(), json_document).ok()) {
                    results.clear();
                    return results;
                }
                results.emplace_back(doc_id, std::move(json_document));
                if (max_count > 0 && results.size() >= max_count) break;
            }
            return results;
        }

// ══════════════════════════════════════════════════════════════
// §14  LookupJoin
// ══════════════════════════════════════════════════════════════

        JoinResult DocEngine::LookupJoin(const std::string& from_collection,
                const std::string& from_field,const std::string& to_collection,
                const std::string& to_field,const nexora::query::Condition& condition,uint32_t limit) {
            try {
                auto result=LookupJoinImpl(from_collection,from_field,to_collection,to_field,condition,limit);
                if(!result.success) {std::vector<std::string>{}.swap(result.records);result.memory_lease.reset();}
                return result;
            } catch(const ResourceExhausted& error) {JoinResult result;result.error_msg=error.what();return result;}
        }

        JoinResult DocEngine::LookupJoinImpl(const std::string& from_collection,
                const std::string& from_field,const std::string& to_collection,
                const std::string& to_field,const nexora::query::Condition& condition,uint32_t limit) {
            JoinResult result;
            result.memory_lease=std::make_shared<BudgetLease>(ExecutionBudget(),ResourceBudget::Query,128*1024*1024);
            std::shared_lock ddl_lock(index_ddl_mutex_);
            if (IsReservedCollectionName(from_collection) || IsReservedCollectionName(to_collection)) {
                result.error_msg="reserved collection name"; return result;
            }
            if (!CollectionExists(from_collection) || !CollectionExists(to_collection)) {
                result.error_msg="JOIN collection does not exist"; return result;
            }
            const query::CompiledQuery compiled(condition);
            const query::CompiledFieldPath path(from_field);
            if (!compiled.valid() || !path.valid()) { result.error_msg="Invalid JOIN predicate/path"; return result; }
            ReadEpoch epoch(txn_db_.get()); auto read_epoch=read_options_; read_epoch.snapshot=epoch.get();
            const std::string prefix=std::string(keys::kData)+from_collection+":";
            const auto upper_value=PrefixUpperBound(prefix); const rocksdb::Slice upper(upper_value);
            auto scan=MakeScanReadOptions(read_epoch); scan.iterate_upper_bound=&upper;
            auto iterator=std::unique_ptr<rocksdb::Iterator>(txn_db_->NewIterator(scan));
            struct Candidate { std::string document,value; query::ValueType type; };
            iterator->Seek(prefix); bool finished=false;
            size_t output_bytes=0;
            while (iterator->Valid() && !finished) {
                std::vector<Candidate> batch; batch.reserve(64);
                while (iterator->Valid() && batch.size()<1) {
                    if(iterator->value().size()>1024*1024) {result.error_msg="JOIN record quota exceeded (1MiB stored)";return result;}
                    const auto stored=iterator->value().ToString(); iterator->Next();
                    bool matched=false; auto status=MatchDocument(stored,compiled,matched);
                    if (!status.ok()) { result.error_msg=status.ToString(); return result; }
                    if (!matched) continue;
                    std::string document; status=DecodeStoredDocument(stored,document);
                    if (!status.ok()) { result.error_msg=status.ToString(); return result; }
                    const auto encoded=DocumentCodec::EncodeJson(document);
                    if (!encoded.success) { result.error_msg=encoded.error; return result; }
                    query::DocumentView view(std::string_view(encoded.value).substr(DocumentCodec::kEnvelopeSize));
                    const auto value=view.Get(path); query::ValueType type;
                    switch (value.type) {
                        case query::DocumentValueType::String: type=query::ValueType::String; break;
                        case query::DocumentValueType::Int64: type=query::ValueType::Int64; break;
                        case query::DocumentValueType::Float64: type=query::ValueType::Float64; break;
                        case query::DocumentValueType::Bool: type=query::ValueType::Bool; break;
                        default: continue;
                    }
                    if (to_field=="_id" && type!=query::ValueType::String) continue;
                    batch.push_back({std::move(document),ScalarToString(value),type});
                }
                std::vector<std::string> targets;
                std::vector<rocksdb::Status> statuses;
                if (to_field=="_id" && !batch.empty()) {
                    std::vector<std::string> keys; keys.reserve(batch.size());
                    for (const auto& row:batch) keys.push_back(MakeDocKey(to_collection,row.value));
                    std::vector<rocksdb::Slice> slices; slices.reserve(keys.size());
                    for (const auto& key:keys) slices.emplace_back(key);
                    ++multiget_calls_; multiget_keys_+=keys.size();
                    statuses=txn_db_->MultiGet(read_epoch,slices,&targets);
                }
                for (size_t index=0;index<batch.size();++index) {
                    std::string target;
                    if (to_field=="_id") {
                        if (statuses[index].IsNotFound()) continue;
                        if (!statuses[index].ok()) { result.error_msg=statuses[index].ToString(); return result; }
                        if(targets[index].size()>1024*1024) {result.error_msg="JOIN target record quota exceeded (1MiB stored)";return result;}
                        const auto status=DecodeStoredDocument(targets[index],target);
                        if (!status.ok()) { result.error_msg=status.ToString(); return result; }
                    } else {
                        // Preserve existing first-match inner lookup semantics.
                        const auto query=ExecuteQuery(to_collection,query::Condition::Leaf(to_field,query::Op::EQ,
                            batch[index].value,batch[index].type),1,0,false,epoch.get(),true).result;
                        if (!query.success) { result.error_msg=query.error_msg; return result; }
                        const auto rows=nlohmann::ordered_json::parse(query.data,nullptr,false);
                        if (!rows.is_array()) { result.error_msg="Invalid JOIN result"; return result; }
                        if (rows.empty()) continue;
                        target=rows.front().dump();
                    }
                    auto source=nlohmann::ordered_json::parse(batch[index].document,nullptr,false);
                    const auto joined=nlohmann::ordered_json::parse(target,nullptr,false);
                    if (!source.is_object() || !joined.is_object()) { result.error_msg="Invalid JOIN document"; return result; }
                    source["__joined__"]=joined;
                    auto merged=source.dump();
                    if(merged.size()>16*1024*1024) {result.error_msg="JOIN result quota exceeded";return result;}
                    output_bytes+=merged.size()+sizeof(std::string);
                    if(output_bytes>16*1024*1024) {result.error_msg="JOIN result quota exceeded (16MiB)";return result;}
                    result.records.push_back(std::move(merged));
                    if (limit && result.records.size()>=limit) { finished=true; break; }
                }
            }
            if (!iterator->status().ok()) { result.error_msg=iterator->status().ToString(); return result; }
            result.success=true; return result;
        }

// ══════════════════════════════════════════════════════════════
// §15  مدیریت Transaction
// ══════════════════════════════════════════════════════════════

        std::unique_ptr<TxHandle> DocEngine::BeginTransaction() {
            TouchUserActivity();
            rocksdb::WriteOptions wo=write_options_;
            rocksdb::Transaction* tx = txn_db_->BeginTransaction(
                    wo,
                    transaction_options_);
            if (!tx) {
                NX_LOG("BeginTransaction: failed");
                return nullptr;
            }
            return std::make_unique<TxHandle>(tx, index_ddl_mutex_,ExecutionBudget());
        }

        DBResult DocEngine::CommitTransaction(TxHandle& tx_handle) {
            CheckExecution();
            if (!tx_handle.IsValid())
                return DBResult::Err("Invalid transaction handle");
            rocksdb::Status s;
            {WriteStageTimer timer(operations_,OperationsState::CommitStage);s=tx_handle.Get()->Commit();}
            if (!s.ok()) {
                {
                    std::lock_guard lock(operations_.mutex);
                    operations_.io_error |= s.IsIOError();
                    operations_.corruption |= s.IsCorruption();
                }
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
                {
                    std::lock_guard lock(operations_.mutex);
                    operations_.io_error |= s.IsIOError();
                    operations_.corruption |= s.IsCorruption();
                }
                tx_handle.Reset();
                return RocksFailure("RollbackTransaction", s);
            }
            tx_handle.Reset();
            return DBResult::Ok("Transaction rolled back");
        }

        DBResult DocEngine::InsertOneTx(TxHandle&          tx_handle,
                                        const std::string& collection_name,
                                        const std::string& input_document) {
            std::string bson_document = input_document;
            if (!tx_handle.IsValid()) return DBResult::Err("Invalid transaction");
            const auto admitted=tx_handle.Admit(input_document.size());
            if(!admitted.success) return admitted;
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

            std::string default_error;
            if (!ApplyInsertDefaults(bson_document, metadata.schema, default_error))
                return DBResult::Err(default_error, "validation_failed");

            if (metadata.schema.strict || !metadata.schema.fields.empty()) {
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
            if (doc_id.size() > 1024)
                return DBResult::Err(
                        "Document _id exceeds the 1 KiB limit");

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

            std::string new_document;
            status = ApplyUpdate(
                    old_document, update_spec, new_document);
            if (!status.ok())
                return RocksFailure("UpdateByIdTx apply update", status);

            if (metadata.schema.strict || !metadata.schema.fields.empty()) {
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

            const auto admitted=tx_handle.Admit(old_document.size()+new_document.size());
            if(!admitted.success) return admitted;
            status = mutation.ReplaceDocument(doc_id,old_document,new_document);
            if (!status.ok()) return RocksFailure("UpdateByIdTx document/index staging",status);

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

            const auto admitted=tx_handle.Admit(old_document.size()+doc_id.size());
            if(!admitted.success) return admitted;
            status = mutation.DeleteDocument(doc_id,old_document);
            if (!status.ok()) return RocksFailure("DeleteByIdTx document/index staging",status);

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
                                       const std::string& doc_id,bool for_update) {
            if (!tx_handle.IsValid()) return DBResult::Err("Invalid transaction");
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");

            std::string doc_key = MakeDocKey(collection_name, doc_id);
            std::string bson;
            if(for_update) {
                const auto admitted=tx_handle.Admit(doc_id.size());
                if(!admitted.success) return admitted;
                const auto guard=LockConstraintGuard(tx_handle.Get());
                if(!guard.ok()) return RocksFailure("FindByIdTx constraint guard",guard);
            }
            rocksdb::Status s = for_update ?
                tx_handle.Get()->GetForUpdate(read_options_,doc_key,&bson) :
                tx_handle.Get()->Get(read_options_, doc_key, &bson);
            if (s.IsNotFound()) return DBResult::Err("Document not found: " + doc_id, "not_found");
            if(!s.ok()) return RocksFailure("FindByIdTx",s);
            try {
                if(bson.size()>(SIZE_MAX-4096)/8) throw ResourceExhausted("transaction read overflow");
                BudgetLease scratch(ExecutionBudget(),ResourceBudget::Buffer,bson.size()*8+4096);
                std::string json;
                s = DecodeStoredDocument(bson, json);
                if (!s.ok()) return RocksFailure("FindByIdTx decode", s);
                auto credit=std::make_shared<BudgetLease>(ExecutionBudget(),ResourceBudget::Query,json.capacity());
                auto result=DBResult::Ok(std::move(json));result.memory_lease=std::move(credit);return result;
            } catch(const ResourceExhausted& error) {return DBResult::Err(error.what(),"resource_exhausted");}
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
                std::string value;
                const auto status = txn_db_->Get(read_options_,
                        std::string(keys::kSeq) + collection_name, &value);
                if (!status.ok() && !status.IsNotFound()) {
                    std::lock_guard lock(operations_.mutex);
                    operations_.io_error |= status.IsIOError();
                    operations_.corruption |= status.IsCorruption();
                    return RocksFailure("Count counter read", status);
                }
                std::size_t parsed = 0;
                std::int64_t size = -1;
                try { size = std::stoll(value, &parsed); } catch (...) {}
                if (status.IsNotFound() || parsed != value.size() || size < 0 ||
                    value.find_first_not_of("0123456789") != std::string::npos) {
                    std::lock_guard lock(operations_.mutex);
                    operations_.corruption = true;
                    return RocksFailure("Count counter decode", rocksdb::Status::Corruption("invalid or missing document counter"));
                }
                return DBResult::Ok(std::to_string(size));
            }

            const QueryExecution execution = ExecuteQuery(
                    collection_name, condition, 0, 0, false,nullptr,false,true);
            if (!execution.result.success) return execution.result;
            return DBResult::Ok(std::to_string(execution.matched_documents));
        }

        DBResult DocEngine::Exists(const std::string&              collection_name,
                                   const nexora::query::Condition& condition) {
            if (IsReservedCollectionName(collection_name))
                return DBResult::Err("reserved collection name");
            if (!CollectionExists(collection_name))
                return DBResult::Err("Collection '" + collection_name + "' does not exist");

            const QueryExecution execution = ExecuteQuery(
                    collection_name, condition, 1, 0, false);
            if (!execution.result.success) return execution.result;
            return DBResult::Ok(
                    execution.matched_documents > 0 ? "true" : "false");
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
            std::string decoded;
            s = DecodeStoredDocument(user_json, decoded);
            if (!s.ok()) return RocksFailure("GetInternalUser decode", s);
            return DBResult::Ok(std::move(decoded));
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
            std::string deleted_json;
            s = ApplyUpdate(existing, spec, deleted_json);
            if (!s.ok()) return RocksFailure("DeleteInternalUser apply update", s);

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
                    txn_db_->NewIterator(MakeScanReadOptions(read_options_)));
            std::string json = "[";
            bool first = true;
            for (iterator->Seek(prefix);
                 iterator->Valid() && iterator->key().starts_with(prefix);
                 iterator->Next()) {
                if (!first) json += ',';
                first = false;
                std::string document;
                const rocksdb::Status decode_status = DecodeStoredDocument(
                        iterator->value().ToString(), document);
                if (!decode_status.ok()) {
                    return RocksFailure(
                            "ListInternalAppTokens decode", decode_status);
                }
                json += document;
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
            MutationMetadata metadata;
            if (!LoadMutationMetadata(collection_name,metadata).ok()) return std::nullopt;
            return metadata.schema;
        }

        std::vector<ForeignKeyDefinition>
        DocEngine::GetForeignKeys(const std::string& collection_name) const {
            if (IsReservedCollectionName(collection_name)) return {};
            MutationMetadata metadata;
            if (!LoadMutationMetadata(collection_name,metadata).ok()) return {};
            return metadata.foreign_keys;
        }

        std::vector<IndexDefinition>
        DocEngine::GetIndexes(const std::string& collection_name) const {
            if (IsReservedCollectionName(collection_name)) return {};
            MutationMetadata metadata;
            if (!LoadMutationMetadata(collection_name,metadata).ok()) return {};
            return metadata.indexes;
        }

        std::vector<std::string> DocEngine::ListCollections() const {
            std::vector<std::string> result;
            std::string prefix = keys::kMetaCol;
            rocksdb::ReadOptions ro=MakeScanReadOptions(read_options_);
            PrefixIterator it(txn_db_.get(),prefix,ro);
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

        RocksMetrics DocEngine::GetRocksMetrics() const {
            RocksMetrics result;
            result.cache_capacity=rocks_resources_->cache->GetCapacity();
            result.cache_usage=rocks_resources_->cache->GetUsage();
            result.cache_pinned=rocks_resources_->cache->GetPinnedUsage();
            result.memtable_budget=rocks_resources_->memtable_bytes;
            result.memtable_usage=rocks_resources_->write_buffers->memory_usage();
            auto property=[&](const char* name,uint64_t& value) {
                if (!txn_db_->GetIntProperty(name,&value)) {
                    std::string text;
                    if (txn_db_->GetProperty(name,&text)) (void)std::from_chars(text.data(),text.data()+text.size(),value);
                }
            };
            property("rocksdb.estimate-pending-compaction-bytes",result.pending_compaction_bytes);
            property("rocksdb.live-sst-files-size",result.live_sst_bytes);
            property("rocksdb.total-sst-files-size",result.total_sst_bytes);
            property("rocksdb.num-files-at-level0",result.l0_files);
            property("rocksdb.is-write-stopped",result.write_stopped);
            property("rocksdb.actual-delayed-write-rate",result.delayed_write_rate);
            property("rocksdb.num-running-compactions",result.running_compactions);
            property("rocksdb.num-running-flushes",result.running_flushes);
            const auto& stats=db_options_.statistics;
            result.multiget_calls=multiget_calls_; result.multiget_keys=multiget_keys_;
            result.statistics_enabled=bool(stats);
            if (!stats) return result; // Properties and engine counters remain available.
            result.logical_write_bytes=stats->getTickerCount(rocksdb::BYTES_WRITTEN);
            result.wal_write_bytes=stats->getTickerCount(rocksdb::WAL_FILE_BYTES);
            result.flush_write_bytes=stats->getTickerCount(rocksdb::FLUSH_WRITE_BYTES);
            result.compaction_write_bytes=stats->getTickerCount(rocksdb::COMPACT_WRITE_BYTES);
            result.stall_micros=stats->getTickerCount(rocksdb::STALL_MICROS);
            result.cache_hits=stats->getTickerCount(rocksdb::BLOCK_CACHE_HIT);
            result.cache_misses=stats->getTickerCount(rocksdb::BLOCK_CACHE_MISS);
            result.bloom_useful=stats->getTickerCount(rocksdb::BLOOM_FILTER_USEFUL);
            result.bloom_full_positive=stats->getTickerCount(rocksdb::BLOOM_FILTER_FULL_POSITIVE);
            result.bloom_prefix_checked=stats->getTickerCount(rocksdb::BLOOM_FILTER_PREFIX_CHECKED);
            result.bloom_prefix_useful=stats->getTickerCount(rocksdb::BLOOM_FILTER_PREFIX_USEFUL);
            result.get_read_bytes=stats->getTickerCount(rocksdb::BYTES_READ);
            result.multiget_read_bytes=stats->getTickerCount(rocksdb::NUMBER_MULTIGET_BYTES_READ);
            result.iterator_read_bytes=stats->getTickerCount(rocksdb::ITER_BYTES_READ);
            result.compaction_read_bytes=stats->getTickerCount(rocksdb::COMPACT_READ_BYTES);
            const auto ratio=[](uint64_t numerator,uint64_t other) {
                const double total=double(numerator)+double(other);
                return total ? double(numerator)/total : 0.0;
            };
            result.cache_hit_rate=ratio(result.cache_hits,result.cache_misses);
            result.bloom_useful_rate=ratio(result.bloom_useful,result.bloom_full_positive);
            if (result.bloom_prefix_checked) result.bloom_prefix_useful_rate=
                double(result.bloom_prefix_useful)/double(result.bloom_prefix_checked);
            if (result.logical_write_bytes) result.sst_write_amplification=
                double(result.flush_write_bytes+result.compaction_write_bytes)/double(result.logical_write_bytes);
            return result;
        }
        DBResult DocEngine::FlushStorage() {
            rocksdb::FlushOptions options; options.wait=true;
            const auto status=txn_db_->Flush(options);
            if(!status.ok()) {
                std::lock_guard lock(operations_.mutex);
                operations_.io_error|=status.IsIOError(); operations_.corruption|=status.IsCorruption();
            }
            return status.ok() ? DBResult::Ok() : RocksFailure("Flush",status);
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
