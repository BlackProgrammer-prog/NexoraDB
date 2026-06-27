//
// Created by HOME on 6/4/2026.
//

#ifndef NEXORADB_DOCENGINE_H
#define NEXORADB_DOCENGINE_H

#pragma once

/**
 * @file DocEngine.h
 * @brief NexoraDB - Core Document Engine (NoSQL Layer)
 *
 * @details
 * این فایل هسته اصلی موتور اسناد NexoraDB است. مسئولیت‌های اصلی:
 *  - مدیریت Collections و Schema Validation
 *  - عملیات CRUD با پشتیبانی از BSON
 *  - مدیریت Index و Foreign Key
 *  - مدیریت Transaction (ACID از طریق RocksDB)
 *  - Internal API برای GraphEngine (Iterator سریع روی Collections)
 *
 * @note
 * داده‌ها به فرمت BSON باینری در RocksDB ذخیره می‌شوند.
 * طراحی به گونه‌ای است که GraphEngine بتواند مستقیماً از DocEngine استفاده کند.
 *
 * @architecture
 * ┌─────────────────────────────────────────────────────┐
 * │             Python / Cython Layer                   │
 * ├─────────────────────────────────────────────────────┤
 * │         GraphEngine (in-memory graph)               │
 * │   از DocEngine برای Startup بارگذاری می‌کند         │
 * ├─────────────────────────────────────────────────────┤
 * │              DocEngine (این فایل)                   │
 * │         ذخیره‌سازی، CRUD، Index، Transaction        │
 * ├────────────────────────┬────────────────────────────┤
 * │      QueryLayer        │        RocksDB             │
 * │ Condition/UpdateSpec   │   (Key-Value, BSON)        │
 * │ Evaluator (Match/Apply)│                            │
 * └────────────────────────┴────────────────────────────┘
 */

// ─── QueryLayer (باید قبل از هر چیز دیگری include شود) ───
#include "query/Condition.h"
#include "query/UpdateSpec.h"
#include "query/Evaluator.h"

// ─── RocksDB ───
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/status.h>
#include <rocksdb/transaction_log.h>
#include <rocksdb/utilities/transaction.h>
#include <rocksdb/utilities/transaction_db.h>
#include <rocksdb/write_batch.h>

// ─── Standard Library ───
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// ─────────────────────────────────────────────────────────────
// Namespace: nexora::core
// ─────────────────────────────────────────────────────────────

namespace nexora {
    namespace core {

// ══════════════════════════════════════════════════════════════
// §1  خروجی استاندارد عملیات
// ══════════════════════════════════════════════════════════════

/**
 * @struct DBResult
 * @brief خروجی استاندارد تمام توابع عمومی DocEngine
 *
 * @field success   آیا عملیات موفق بود؟
 * @field data      خروجی BSON/JSON به صورت رشته (در صورت موفقیت)
 * @field error_msg پیام خطا (در صورت شکست)
 *
 * @example استفاده در Cython:
 *   result = engine.FindById("users", "u_001")
 *   if result.success:
 *       process(result.data)
 *   else:
 *       log_error(result.error_msg)
 */
        struct DBResult {
            bool        success   = false;
            std::string data      = "";  ///< BSON hex یا JSON string
            std::string error_msg = "";

            /// سازنده‌های کمکی برای راحتی ساخت نتیجه
            static DBResult Ok(std::string payload = "") {
                return {true, std::move(payload), ""};
            }
            static DBResult Err(std::string msg) {
                return {false, "", std::move(msg)};
            }
        };

// ══════════════════════════════════════════════════════════════
// §2  تعریف Schema و Index
// ══════════════════════════════════════════════════════════════

/**
 * @enum FieldType
 * @brief نوع فیلدهای Schema
 */
        enum class FieldType : uint8_t {
            String  = 0,
            Int32   = 1,
            Int64   = 2,
            Float64 = 3,
            Bool    = 4,
            Array   = 5,
            Object  = 6,
            Binary  = 7,
            Null    = 8
        };

/**
 * @struct SchemaField
 * @brief تعریف یک فیلد در Schema
 *
 * @field name        نام فیلد
 * @field type        نوع داده
 * @field required    آیا الزامی است؟
 * @field unique      آیا باید یکتا باشد؟
 * @field default_val مقدار پیش‌فرض (اختیاری)
 */
        struct SchemaField {
            std::string                name;
            FieldType                  type       = FieldType::String;
            bool                       required   = false;
            bool                       unique     = false;
            std::optional<std::string> default_val;
        };

/**
 * @struct SchemaDefinition
 * @brief مجموعه‌ای از فیلدها که Schema یک Collection را می‌سازند
 *
 * @field strict اگر true باشد، فیلدهای ناشناخته رد می‌شوند
 */
        struct SchemaDefinition {
            std::vector<SchemaField> fields;
            bool                     strict = false;
        };

/**
 * @enum IndexType
 * @brief نوع Index
 */
        enum class IndexType : uint8_t {
            SingleField = 0, ///< ایندکس روی یک فیلد
            Compound    = 1, ///< ایندکس ترکیبی (چند فیلد)
            Unique      = 2  ///< ایندکس یکتا
        };

/**
 * @struct IndexDefinition
 * @brief تعریف یک Index
 *
 * @field index_name نام ایندکس (باید در Collection یکتا باشد)
 * @field fields     فیلدهای شامل در ایندکس
 * @field type       نوع ایندکس
 */
        struct IndexDefinition {
            std::string              index_name;
            std::vector<std::string> fields;
            IndexType                type = IndexType::SingleField;
        };

/**
 * @struct ForeignKeyDefinition
 * @brief تعریف یک کلید خارجی
 *
 * @field fk_name        نام کلید خارجی
 * @field local_field    فیلد محلی
 * @field ref_collection Collection مرجع
 * @field ref_field      فیلد مرجع (معمولاً "_id")
 *
 * @graph_note
 * GraphEngine از این تعاریف برای شناخت خودکار edge types استفاده می‌کند:
 * هر ForeignKey = یک نوع edge در گراف.
 * مثال: posts.author_id → users._id = edge نوع "Authored"
 */
        struct ForeignKeyDefinition {
            std::string fk_name;
            std::string local_field;
            std::string ref_collection;
            std::string ref_field = "_id";
        };

// ══════════════════════════════════════════════════════════════
// §3  Internal Graph Iterator API
// ══════════════════════════════════════════════════════════════

/**
 * @brief نوع callback برای iteration روی Documents
 *
 * @details
 * این callback توسط GraphEngine در هنگام Startup برای ساخت گراف استفاده می‌شود.
 * پارامترها:
 *   - doc_id:    شناسه سند
 *   - bson_data: محتوای BSON باینری سند
 *
 * اگر callback مقدار false برگرداند، iteration متوقف می‌شود (early exit).
 *
 * @example استفاده در GraphEngine::BuildGraph():
 * ```cpp
 * engine.IterateCollection("users",
 *     [&](const std::string& id, const std::string& bson) -> bool {
 *         graph.AddNode(id, bson);
 *         return true; // ادامه بده
 *     });
 * ```
 */
        using DocumentCallback = std::function<bool(const std::string& doc_id,
                                                    const std::string& bson_data)>;

// ══════════════════════════════════════════════════════════════
// §4  Transaction Handle
// ══════════════════════════════════════════════════════════════

/**
 * @class TxHandle
 * @brief هندل تراکنش که توسط BeginTransaction برگردانده می‌شود
 *
 * @details
 * این کلاس یک wrapper ساده برای rocksdb::Transaction است.
 * کاربر (یا Cython layer) باید این هندل را نگهداری کند و
 * به توابع Tx پاس دهد.
 *
 * @note عمر این شیء باید توسط caller مدیریت شود.
 */
        class TxHandle {
        public:
            explicit TxHandle(rocksdb::Transaction* tx) : tx_(tx) {}
            ~TxHandle() = default;

            // non-copyable, movable
            TxHandle(const TxHandle&)            = delete;
            TxHandle& operator=(const TxHandle&) = delete;
            TxHandle(TxHandle&&)                 = default;
            TxHandle& operator=(TxHandle&&)      = default;

            rocksdb::Transaction* Get()     const noexcept { return tx_; }
            bool                  IsValid() const noexcept { return tx_ != nullptr; }

        private:
            rocksdb::Transaction* tx_ = nullptr;
        };

// ══════════════════════════════════════════════════════════════
// §5  LookupJoin Result
// ══════════════════════════════════════════════════════════════

/**
 * @struct JoinResult
 * @brief نتیجه یک عملیات LookupJoin
 *
 * @field success نشان‌دهنده موفقیت عملیات
 * @field records لیست اسناد Join شده به صورت BSON string
 * @field error_msg پیام خطا در صورت شکست
 */
        struct JoinResult {
            bool                     success   = false;
            std::vector<std::string> records;  ///< هر عنصر یک سند BSON merge شده است
            std::string              error_msg;
        };

// ══════════════════════════════════════════════════════════════
// §6  کلاس اصلی DocEngine
// ══════════════════════════════════════════════════════════════

/**
 * @class DocEngine
 * @brief موتور اصلی پایگاه داده اسناد NexoraDB
 *
 * @details
 * DocEngine تمام عملیات CRUD، مدیریت Schema، Index و Transaction را
 * روی RocksDB پیاده‌سازی می‌کند. این کلاس:
 *
 *  1. **Thread-safe نیست** — هر thread باید instance جداگانه داشته باشد
 *     یا از locking خارجی استفاده شود (Cython layer مسئول این است).
 *  2. **Internal API** برای GraphEngine از طریق `IterateCollection` فراهم است.
 *  3. تمام داده‌ها BSON باینری هستند ولی API رشته BSON hex یا raw bytes می‌پذیرد.
 *  4. منطق تطابق (Match) و به‌روزرسانی (Apply) به nexora::query::Evaluator
 *     واگذار شده است — DocEngine فقط ذخیره‌سازی و iteration را مدیریت می‌کند.
 *
 * @section key_format فرمت کلیدهای RocksDB
 * ```
 * data:{collection}:{doc_id}        → BSON سند
 * meta:col:{name}                   → SchemaDefinition serialize شده
 * meta:idx:{collection}:{name}      → IndexDefinition serialize شده
 * meta:fk:{collection}:{name}       → ForeignKeyDefinition serialize شده
 * idx:{collection}:{field}:{value}:{doc_id} → doc_id (برای index lookup)
 * seq:{collection}                  → شمارنده اسناد
 * ```
 */
        class DocEngine {
        public:
            // ──────────────────────────────────────────────────────────
            // 6.1  چرخه حیات (Lifecycle)
            // ──────────────────────────────────────────────────────────

            /**
             * @brief سازنده - RocksDB را باز می‌کند
             * @param db_path مسیر دایرکتوری RocksDB
             *
             * @throws std::runtime_error اگر باز کردن دیتابیس شکست بخورد
             *
             * @example
             *   auto engine = std::make_unique<DocEngine>("/data/nexoradb");
             */
            explicit DocEngine(const std::string& db_path);

            /**
             * @brief مخرب - اتصال RocksDB را می‌بندد
             */
            ~DocEngine();

            // non-copyable
            DocEngine(const DocEngine&)            = delete;
            DocEngine& operator=(const DocEngine&) = delete;

            /**
             * @brief بررسی سلامت اتصال به دیتابیس
             * @return true اگر RocksDB سالم و قابل استفاده باشد
             */
            bool IsHealthy() const noexcept;

            // ──────────────────────────────────────────────────────────
            // 6.2  مدیریت Collection و Schema
            // ──────────────────────────────────────────────────────────

            /**
             * @brief یک Collection جدید ایجاد می‌کند
             * @param collection_name نام Collection (مثلاً "users")
             * @param schema          (اختیاری) تعریف Schema اولیه
             * @return DBResult با success=true اگر موفق باشد
             *
             * @note اگر Collection از قبل وجود داشته باشد، خطا برمی‌گرداند.
             *
             * @example
             *   SchemaDefinition schema;
             *   schema.fields.push_back({"username", FieldType::String, true, true});
             *   schema.fields.push_back({"email",    FieldType::String, true, true});
             *   auto r = engine.CreateCollection("users", schema);
             */
            DBResult CreateCollection(const std::string&                     collection_name,
                                      const std::optional<SchemaDefinition>& schema = std::nullopt);

            /**
             * @brief یک Collection و تمام داده‌های آن را حذف می‌کند
             * @param collection_name نام Collection
             * @return DBResult
             *
             * @warning این عملیات برگشت‌ناپذیر است.
             */
            DBResult DropCollection(const std::string& collection_name);

            /**
             * @brief Schema Validation یک Collection را تنظیم یا به‌روز می‌کند
             * @param collection_name نام Collection
             * @param schema          تعریف Schema جدید
             * @return DBResult
             *
             * @note این تابع Schema موجود را جایگزین می‌کند (overwrite).
             */
            DBResult SetSchemaValidation(const std::string&      collection_name,
                                         const SchemaDefinition& schema);

            /**
             * @brief بررسی وجود یک Collection
             * @param collection_name نام Collection
             * @return true اگر Collection وجود داشته باشد
             */
            bool CollectionExists(const std::string& collection_name) const;

            // ──────────────────────────────────────────────────────────
            // 6.3  مدیریت Index
            // ──────────────────────────────────────────────────────────

            /**
             * @brief یک Index جدید روی Collection ایجاد می‌کند
             * @param collection_name نام Collection
             * @param index_def       تعریف Index
             * @return DBResult
             *
             * @details
             * پس از ایجاد Index، تمام اسناد موجود بازنویسی می‌شوند (index rebuild).
             *
             * @example
             *   IndexDefinition idx;
             *   idx.index_name = "idx_username";
             *   idx.fields     = {"username"};
             *   idx.type       = IndexType::Unique;
             *   engine.CreateIndex("users", idx);
             */
            DBResult CreateIndex(const std::string&     collection_name,
                                 const IndexDefinition& index_def);

            /**
             * @brief یک Index را حذف می‌کند
             * @param collection_name نام Collection
             * @param index_name      نام Index
             * @return DBResult
             */
            DBResult DropIndex(const std::string& collection_name,
                               const std::string& index_name);

            // ──────────────────────────────────────────────────────────
            // 6.4  مدیریت Foreign Key
            // ──────────────────────────────────────────────────────────

            /**
             * @brief یک Foreign Key تعریف می‌کند
             * @param collection_name نام Collection مبدا
             * @param fk_def          تعریف Foreign Key
             * @return DBResult
             *
             * @details
             * Foreign Key‌ها در InsertOne/InsertMany و UpdateById/UpdateMany بررسی می‌شوند.
             *
             * @example (ارتباط posts → users)
             *   ForeignKeyDefinition fk;
             *   fk.fk_name        = "fk_author";
             *   fk.local_field    = "author_id";
             *   fk.ref_collection = "users";
             *   fk.ref_field      = "_id";
             *   engine.AddForeignKey("posts", fk);
             *
             * @graph_note
             *   GraphEngine از FK‌ها برای تشخیص خودکار edge types استفاده می‌کند.
             *   هر FK = یک نوع edge در گراف.
             */
            DBResult AddForeignKey(const std::string&          collection_name,
                                   const ForeignKeyDefinition& fk_def);

            /**
             * @brief یک Foreign Key را حذف می‌کند
             * @param collection_name نام Collection
             * @param fk_name         نام Foreign Key
             * @return DBResult
             */
            DBResult DropForeignKey(const std::string& collection_name,
                                    const std::string& fk_name);

            // ──────────────────────────────────────────────────────────
            // 6.5  CRUD - Insert
            // ──────────────────────────────────────────────────────────

            /**
             * @brief یک سند جدید درج می‌کند
             * @param collection_name نام Collection
             * @param bson_document   محتوای سند به فرمت BSON باینری
             * @return DBResult با data برابر doc_id سند ایجاد شده
             *
             * @details
             * - اگر سند شامل "_id" نباشد، یک UUID v4 اختصاص داده می‌شود.
             * - Schema Validation اعمال می‌شود.
             * - Index‌ها به‌روز می‌شوند.
             * - Foreign Key‌ها بررسی می‌شوند.
             *
             * @example
             *   std::string bson = R"({"username":"alice","email":"alice@test.com"})";
             *   auto r = engine.InsertOne("users", bson);
             *   std::string new_id = r.data;
             */
            DBResult InsertOne(const std::string& collection_name,
                               const std::string& bson_document);

            /**
             * @brief چندین سند را در یک عملیات اتمیک درج می‌کند
             * @param collection_name نام Collection
             * @param bson_documents  لیست اسناد BSON
             * @return DBResult با data برابر JSON آرایه doc_id‌های ایجاد شده
             *
             * @details از RocksDB WriteBatch استفاده می‌کند — اتمیک است.
             */
            DBResult InsertMany(const std::string&              collection_name,
                                const std::vector<std::string>& bson_documents);

            // ──────────────────────────────────────────────────────────
            // 6.6  CRUD - Find
            // ──────────────────────────────────────────────────────────

            /**
             * @brief یک سند را بر اساس ID بازیابی می‌کند
             * @param collection_name نام Collection
             * @param doc_id          شناسه سند
             * @return DBResult با data برابر محتوای BSON سند
             *
             * @details O(1) — مستقیماً از RocksDB با key می‌خواند.
             */
            DBResult FindById(const std::string& collection_name,
                              const std::string& doc_id);

            /**
             * @brief اسناد را بر اساس شروط جستجو می‌کند
             * @param collection_name نام Collection
             * @param condition       شرط جستجو (از nexora::query::Condition)
             * @param limit           حداکثر تعداد نتایج (0 = بدون محدودیت)
             * @param skip            تعداد نتایج برای skip (pagination)
             * @return DBResult با data برابر JSON آرایه اسناد
             *
             * @details
             * ارزیابی شرط از طریق nexora::query::Evaluator انجام می‌شود.
             * اگر Condition روی فیلد indexed باشد، از Index استفاده می‌کند.
             */
            DBResult FindMany(const std::string&              collection_name,
                              const nexora::query::Condition& condition,
                              uint32_t                        limit = 0,
                              uint32_t                        skip  = 0);

            // ──────────────────────────────────────────────────────────
            // 6.7  CRUD - Update
            // ──────────────────────────────────────────────────────────

            /**
             * @brief یک سند را بر اساس ID به‌روز می‌کند
             * @param collection_name نام Collection
             * @param doc_id          شناسه سند
             * @param update_spec     مشخصات به‌روزرسانی (از nexora::query::UpdateSpec)
             * @return DBResult با data برابر "1" (موفق) یا "0"
             *
             * @details
             * اعمال update از طریق nexora::query::Evaluator::Apply انجام می‌شود.
             * Index‌های مرتبط به‌روز می‌شوند.
             */
            DBResult UpdateById(const std::string&               collection_name,
                                const std::string&               doc_id,
                                const nexora::query::UpdateSpec& update_spec);

            /**
             * @brief چندین سند منطبق با شرط را به‌روز می‌کند
             * @param collection_name نام Collection
             * @param condition       شرط انتخاب اسناد
             * @param update_spec     مشخصات به‌روزرسانی
             * @return DBResult با data برابر تعداد اسناد تغییر یافته
             */
            DBResult UpdateMany(const std::string&               collection_name,
                                const nexora::query::Condition&  condition,
                                const nexora::query::UpdateSpec& update_spec);

            // ──────────────────────────────────────────────────────────
            // 6.8  CRUD - Delete
            // ──────────────────────────────────────────────────────────

            /**
             * @brief یک سند را بر اساس ID حذف می‌کند
             * @param collection_name نام Collection
             * @param doc_id          شناسه سند
             * @return DBResult با data برابر "1" (حذف شد) یا "0" (پیدا نشد)
             */
            DBResult DeleteById(const std::string& collection_name,
                                const std::string& doc_id);

            /**
             * @brief چندین سند منطبق با شرط را حذف می‌کند
             * @param collection_name نام Collection
             * @param condition       شرط انتخاب اسناد
             * @return DBResult با data برابر تعداد اسناد حذف شده
             *
             * @warning برگشت‌ناپذیر است. پیشنهاد می‌شود در Transaction انجام شود.
             */
            DBResult DeleteMany(const std::string&              collection_name,
                                const nexora::query::Condition& condition);

            // ──────────────────────────────────────────────────────────
            // 6.9  Internal API برای GraphEngine
            // ──────────────────────────────────────────────────────────

            /**
             * @brief تمام اسناد یک Collection را با callback پردازش می‌کند
             * @param collection_name نام Collection (مثلاً "users" یا "posts")
             * @param callback        تابعی که برای هر سند فراخوانی می‌شود
             * @param batch_size      اندازه batch برای بهینه‌سازی (پیش‌فرض: 100)
             *
             * @details
             * **این تابع برای استفاده GraphEngine در Startup طراحی شده است.**
             *
             * - از RocksDB Iterator با fill_cache=false استفاده می‌کند (مناسب bulk scan).
             * - اگر callback مقدار false برگرداند، iteration زودهنگام متوقف می‌شود.
             * - این تابع DBResult برنمی‌گرداند چون برای internal use است.
             *
             * @example استفاده در GraphEngine::BuildGraph()
             * ```cpp
             * // مرحله ۱: بارگذاری nodes
             * doc_engine_.IterateCollection("users",
             *     [this](const std::string& id, const std::string& bson) -> bool {
             *         nexora::query::Evaluator eval;
             *         auto username = eval.ExtractField(bson, "username");
             *         user_nodes_[id] = {id, username.raw};
             *         return true;
             *     });
             *
             * // مرحله ۲: بارگذاری edges
             * doc_engine_.IterateCollection("posts",
             *     [this](const std::string& id, const std::string& bson) -> bool {
             *         nexora::query::Evaluator eval;
             *         auto author = eval.ExtractField(bson, "author_id");
             *         if (author.found) AddEdge(author.raw, id, "Authored");
             *         return true;
             *     });
             * ```
             */
            void IterateCollection(const std::string&      collection_name,
                                   const DocumentCallback& callback,
                                   uint32_t                batch_size = 100) const;

            /**
             * @brief تعداد اسناد یک Collection را بدون بارگذاری داده برمی‌گرداند
             * @param collection_name نام Collection
             * @return تعداد اسناد (یا -1 در صورت خطا)
             *
             * @details O(1) از شمارنده داخلی.
             *
             * @example در GraphEngine برای pre-allocate:
             *   size_t count = engine.GetCollectionSize("users");
             *   user_nodes_.reserve(count);
             */
            int64_t GetCollectionSize(const std::string& collection_name) const;

            /**
             * @brief یک range از اسناد را بر اساس ID prefix بازیابی می‌کند
             * @param collection_name نام Collection
             * @param id_prefix       پیشوند ID (برای range scan)
             * @param max_count       حداکثر تعداد (0 = همه)
             * @return لیست جفت‌های (doc_id, bson_data)
             *
             * @details
             * برای GraphEngine مفید است که می‌خواهد بخشی از داده‌ها را بارگذاری کند
             * (partial graph load یا shard-based loading).
             */
            std::vector<std::pair<std::string, std::string>>
            GetDocumentRange(const std::string& collection_name,
                             const std::string& id_prefix = "",
                             uint32_t           max_count  = 0) const;

            // ──────────────────────────────────────────────────────────
            // 6.10  LookupJoin
            // ──────────────────────────────────────────────────────────

            /**
             * @brief دو Collection را بر اساس یک فیلد مشترک Join می‌کند
             * @param from_collection Collection مبدا
             * @param from_field      فیلد join در مبدا (مثلاً "author_id")
             * @param to_collection   Collection مقصد
             * @param to_field        فیلد join در مقصد (مثلاً "_id")
             * @param condition       شرط فیلتر روی from_collection
             * @param limit           حداکثر نتایج
             * @return JoinResult با لیست اسناد merge شده
             *
             * @details
             * LEFT LOOKUP JOIN انجام می‌دهد.
             * نتیجه: from_doc + field "__joined__" حاوی to_doc
             *
             * @example
             *   Condition cond = Condition::Leaf("likes", Op::GT, "100", ValueType::Int64);
             *   auto result = engine.LookupJoin("posts", "author_id", "users", "_id", cond, 20);
             */
            JoinResult LookupJoin(const std::string&              from_collection,
                                  const std::string&              from_field,
                                  const std::string&              to_collection,
                                  const std::string&              to_field,
                                  const nexora::query::Condition& condition,
                                  uint32_t                        limit = 0);

            // ──────────────────────────────────────────────────────────
            // 6.11  مدیریت Transaction
            // ──────────────────────────────────────────────────────────

            /**
             * @brief یک Transaction جدید شروع می‌کند
             * @return unique_ptr به TxHandle (در صورت خطا nullptr)
             *
             * @details ACID کامل از طریق rocksdb::TransactionDB.
             *
             * @example
             *   auto tx = engine.BeginTransaction();
             *   engine.InsertOneTx(*tx, "messages", msg_bson);
             *   engine.UpdateByIdTx(*tx, "users", user_id, update_spec);
             *   engine.CommitTransaction(*tx);
             */
            std::unique_ptr<TxHandle> BeginTransaction();

            /**
             * @brief یک Transaction را Commit می‌کند
             * @param tx_handle هندل Transaction
             * @return DBResult
             *
             * @note بعد از Commit، هندل دیگر قابل استفاده نیست.
             */
            DBResult CommitTransaction(TxHandle& tx_handle);

            /**
             * @brief یک Transaction را Rollback می‌کند
             * @param tx_handle هندل Transaction
             * @return DBResult
             */
            DBResult RollbackTransaction(TxHandle& tx_handle);

            // ──────────────────────────────────────────────────────────
            // 6.12  CRUD درون Transaction (Tx variants)
            // ──────────────────────────────────────────────────────────

            /** @brief InsertOne درون یک Transaction */
            DBResult InsertOneTx(TxHandle&          tx_handle,
                                 const std::string& collection_name,
                                 const std::string& bson_document);

            /** @brief UpdateById درون یک Transaction (با GetForUpdate lock) */
            DBResult UpdateByIdTx(TxHandle&                        tx_handle,
                                  const std::string&               collection_name,
                                  const std::string&               doc_id,
                                  const nexora::query::UpdateSpec& update_spec);

            /** @brief DeleteById درون یک Transaction (با GetForUpdate lock) */
            DBResult DeleteByIdTx(TxHandle&          tx_handle,
                                  const std::string& collection_name,
                                  const std::string& doc_id);

            /** @brief FindById درون یک Transaction (با دید به uncommitted تغییرات) */
            DBResult FindByIdTx(TxHandle&          tx_handle,
                                const std::string& collection_name,
                                const std::string& doc_id);

            // ──────────────────────────────────────────────────────────
            // 6.13  توابع کمکی
            // ──────────────────────────────────────────────────────────

            /**
             * @brief تعداد اسناد منطبق با شرط را شمارش می‌کند
             * @param collection_name نام Collection
             * @param condition       شرط (اگر IsEmpty باشد، از شمارنده O(1) استفاده می‌شود)
             * @return DBResult با data برابر تعداد به صورت string
             */
            DBResult Count(const std::string&              collection_name,
                           const nexora::query::Condition& condition);

            /**
             * @brief بررسی وجود حداقل یک سند منطبق با شرط
             * @return DBResult با data برابر "true" یا "false"
             *
             * @details بهینه‌تر از Count — به محض یافتن اولین نتیجه متوقف می‌شود.
             */
            DBResult Exists(const std::string&              collection_name,
                            const nexora::query::Condition& condition);

            // ──────────────────────────────────────────────────────────
            // 6.14  دسترسی به پیکربندی (برای GraphEngine)
            // ──────────────────────────────────────────────────────────

            /**
             * @brief تعریف Schema یک Collection را برمی‌گرداند
             * @return optional<SchemaDefinition> — nullopt اگر وجود نداشته باشد
             *
             * @graph_note GraphEngine از این برای شناخت ساختار node fields استفاده می‌کند.
             */
            std::optional<SchemaDefinition> GetSchema(const std::string& collection_name) const;

            /**
             * @brief تمام Foreign Key‌های یک Collection را برمی‌گرداند
             *
             * @graph_note
             *   GraphEngine از این برای کشف خودکار edge types استفاده می‌کند:
             *   هر FK = یک نوع edge در گراف.
             *   ```cpp
             *   auto fks = engine.GetForeignKeys("posts");
             *   // fks[0] = {fk_author, author_id, users, _id}
             *   // → edge نوع "Authored" از posts.author_id به users._id
             *   ```
             */
            std::vector<ForeignKeyDefinition> GetForeignKeys(const std::string& collection_name) const;

            /**
             * @brief تمام Index‌های یک Collection را برمی‌گرداند
             */
            std::vector<IndexDefinition> GetIndexes(const std::string& collection_name) const;

            /**
             * @brief لیست تمام Collection‌های موجود را برمی‌گرداند
             * @return مثال: ["users", "posts", "messages", "reactions"]
             */
            std::vector<std::string> ListCollections() const;

            /**
             * @brief مقدار RAM فعلی مصرف‌شده توسط process دیتابیس، بر حسب byte.
             *
             * @note در build لینوکسی از RSS پردازش خوانده می‌شود.
             */
            uint64_t GetRamUsageBytes() const;

            /**
             * @brief فضای دیسک اشغال‌شده توسط دایرکتوری RocksDB، بر حسب byte.
             */
            uint64_t GetDiskUsageBytes() const;

        private:
            // ──────────────────────────────────────────────────────────
            // 6.15  پیاده‌سازی‌های داخلی
            // ──────────────────────────────────────────────────────────

            std::string db_path_;

            /// RocksDB TransactionDB (برای ACID کامل)
            std::unique_ptr<rocksdb::TransactionDB,
                    std::function<void(rocksdb::TransactionDB*)>> txn_db_;

            rocksdb::Options              db_options_;
            rocksdb::TransactionDBOptions txn_db_options_;
            rocksdb::WriteOptions         write_options_;
            rocksdb::ReadOptions          read_options_;

            // ─── متدهای کمکی خصوصی ───

            static std::string MakeDocKey(const std::string& collection,
                                          const std::string& doc_id);

            static std::string MakeMetaKey(const std::string& type,
                                           const std::string& collection,
                                           const std::string& name = "");

            static std::string MakeIndexKey(const std::string& collection,
                                            const std::string& field,
                                            const std::string& value,
                                            const std::string& doc_id);

            bool ValidateDocument(const std::string&      bson_document,
                                  const SchemaDefinition& schema,
                                  std::string&            error_out) const;

            bool CheckForeignKey(const ForeignKeyDefinition& fk,
                                 const std::string&          bson_document,
                                 std::string&                error_out);

            rocksdb::Status UpdateIndexesOnInsert(const std::string& collection,
                                                  const std::string& doc_id,
                                                  const std::string& bson_document);

            rocksdb::Status CleanIndexesOnDelete(const std::string& collection,
                                                 const std::string& doc_id,
                                                 const std::string& old_bson);

            /**
             * @brief استخراج مقدار یک فیلد — از Evaluator استفاده می‌کند
             * @note فراخوانی thread_local Evaluator برای جلوگیری از ساخت مکرر
             */
            static std::string ExtractField(const std::string& bson,
                                            const std::string& field_name);

            /**
             * @brief تطابق سند با Condition — به Evaluator::Match واگذار می‌شود
             */
            static bool MatchesCondition(const std::string&              bson,
                                         const nexora::query::Condition& condition);

            /**
             * @brief اعمال UpdateSpec روی سند — به Evaluator::Apply واگذار می‌شود
             */
            static std::string ApplyUpdate(const std::string&               bson,
                                           const nexora::query::UpdateSpec& spec);

            static std::string SerializeSchema(const SchemaDefinition& schema);
            static std::optional<SchemaDefinition> DeserializeSchema(const std::string& bytes);

            static std::string SerializeIndex(const IndexDefinition& idx);
            static std::optional<IndexDefinition> DeserializeIndex(const std::string& bytes);

            static std::string SerializeFk(const ForeignKeyDefinition& fk);
            static std::optional<ForeignKeyDefinition> DeserializeFk(const std::string& bytes);

            static std::string GenerateDocId();
        };

    } // namespace core
} // namespace nexora

#endif //NEXORADB_DOCENGINE_H
