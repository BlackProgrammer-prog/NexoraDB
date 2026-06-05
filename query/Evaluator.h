//
// Created by HOME on 6/5/2026.
//

#ifndef NEXORADB_EVALUATOR_H
#define NEXORADB_EVALUATOR_H

#endif //NEXORADB_EVALUATOR_H

#pragma once

/**
 * @file query/Evaluator.h
 * @brief ارزیاب شروط و اعمال‌کننده Update روی اسناد BSON
 *
 * @details
 * این کلاس قلب Query Layer است:
 *   1. `Match(bson, condition)` → آیا سند با شرط تطابق دارد؟
 *   2. `Apply(bson, update_spec)` → سند جدید با اعمال update_spec
 *   3. `Project(bson, projection, exclude)` → فیلترینگ فیلدها در خروجی
 *
 * @section separation جداسازی مسئولیت
 * ```
 * DocEngine   → مدیریت ذخیره‌سازی، iteration، transaction
 * Evaluator   → منطق تطابق و به‌روزرسانی (بدون وابستگی به RocksDB)
 * ```
 * این جداسازی اجازه می‌دهد Evaluator را مستقل تست کنید.
 *
 * @section bson_note نکته BSON
 * در MVP، اسناد به صورت JSON-like string ذخیره می‌شوند.
 * در نسخه بعدی با libbson جایگزین می‌شود.
 * Evaluator یک `BsonAdapter` interface دارد که در آینده swap می‌شود.
 */

#include "Condition.h"
#include "UpdateSpec.h"

#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace nexora {
    namespace query {

// ══════════════════════════════════════════════════════════════
// §1  نتیجه Extract یک فیلد
// ══════════════════════════════════════════════════════════════

/**
 * @struct FieldValue
 * @brief مقدار استخراج‌شده از یک فیلد سند
 *
 * @details
 * برای مقایسه صحیح نوع، علاوه بر رشته، نوع اصلی را هم نگه می‌داریم.
 */
        struct FieldValue {
            std::string    raw;               ///< مقدار به صورت رشته
            ValueType      type  = ValueType::Null;
            bool           found = false;     ///< آیا فیلد در سند وجود داشت؟

            bool IsNull()   const noexcept { return !found || type == ValueType::Null; }
            bool IsNumber() const noexcept {
                return type == ValueType::Int64 || type == ValueType::Float64;
            }
        };

// ══════════════════════════════════════════════════════════════
// §2  Interface آداپتور BSON
// ══════════════════════════════════════════════════════════════

/**
 * @class IBsonAdapter
 * @brief Interface برای خواندن/نوشتن BSON
 *
 * @details
 * این interface اجازه می‌دهد در آینده با libbson جایگزین شود
 * بدون اینکه Evaluator تغییر کند.
 *
 * در MVP: `JsonAdapter` این interface را پیاده‌سازی می‌کند.
 * در آینده: `LibBsonAdapter` جایگزین می‌شود.
 */
        class IBsonAdapter {
        public:
            virtual ~IBsonAdapter() = default;

            /**
             * @brief یک فیلد را از سند استخراج می‌کند
             * @param doc        محتوای سند (BSON یا JSON string)
             * @param field_path مسیر فیلد (می‌تواند nested باشد: "address.city")
             * @return FieldValue با مقدار و نوع
             */
            virtual FieldValue GetField(const std::string& doc,
                                        const std::string& field_path) const = 0;

            /**
             * @brief یک فیلد را در سند تنظیم می‌کند
             * @param doc        سند ورودی
             * @param field_path مسیر فیلد
             * @param value      مقدار جدید
             * @param vt         نوع مقدار
             * @return سند جدید
             */
            virtual std::string SetField(const std::string& doc,
                                         const std::string& field_path,
                                         const std::string& value,
                                         UpdateValueType    vt) const = 0;

            /**
             * @brief یک فیلد را از سند حذف می‌کند
             */
            virtual std::string UnsetField(const std::string& doc,
                                           const std::string& field_path) const = 0;

            /**
             * @brief یک عنصر به آرایه اضافه می‌کند
             */
            virtual std::string PushToArray(const std::string& doc,
                                            const std::string& field_path,
                                            const std::string& element,
                                            UpdateValueType    vt) const = 0;

            /**
             * @brief عناصر برابر با value را از آرایه حذف می‌کند
             */
            virtual std::string PullFromArray(const std::string& doc,
                                              const std::string& field_path,
                                              const std::string& value) const = 0;

            /**
             * @brief عناصر موجود در values را از آرایه حذف می‌کند
             */
            virtual std::string PullAllFromArray(const std::string& doc,
                                                 const std::string& field_path,
                                                 const std::vector<std::string>& vals) const = 0;

            /**
             * @brief اول یا آخر آرایه را حذف می‌کند
             * @param from_end اگر true: آخر حذف شود، اگر false: اول
             */
            virtual std::string PopArray(const std::string& doc,
                                         const std::string& field_path,
                                         bool               from_end) const = 0;

            /**
             * @brief فقط فیلدهای مشخص را در سند نگه می‌دارد یا حذف می‌کند
             * @param doc     سند
             * @param fields  فیلدها
             * @param exclude اگر true: فیلدها حذف می‌شوند، اگر false: فقط آن‌ها نگه داشته می‌شوند
             */
            virtual std::string Project(const std::string&              doc,
                                        const std::vector<std::string>& fields,
                                        bool                            exclude) const = 0;

            /**
             * @brief نام یک فیلد را عوض می‌کند
             */
            virtual std::string RenameField(const std::string& doc,
                                            const std::string& old_field,
                                            const std::string& new_field) const = 0;
        };

// ══════════════════════════════════════════════════════════════
// §3  پیاده‌سازی MVP: JsonAdapter
// ══════════════════════════════════════════════════════════════

/**
 * @class JsonAdapter
 * @brief پیاده‌سازی IBsonAdapter برای JSON-like string (MVP)
 *
 * @details
 * این پیاده‌سازی با JSON ساده کار می‌کند.
 * در نسخه بعدی با LibBsonAdapter جایگزین می‌شود.
 *
 * @note
 * این کلاس در Evaluator.cpp پیاده‌سازی می‌شود.
 * خارج از این ماژول نیازی به استفاده مستقیم ندارید.
 */
        class JsonAdapter final : public IBsonAdapter {
        public:
            FieldValue  GetField(const std::string& doc,
                                 const std::string& field_path) const override;

            std::string SetField(const std::string& doc,
                                 const std::string& field_path,
                                 const std::string& value,
                                 UpdateValueType    vt) const override;

            std::string UnsetField(const std::string& doc,
                                   const std::string& field_path) const override;

            std::string PushToArray(const std::string& doc,
                                    const std::string& field_path,
                                    const std::string& element,
                                    UpdateValueType    vt) const override;

            std::string PullFromArray(const std::string& doc,
                                      const std::string& field_path,
                                      const std::string& value) const override;

            std::string PullAllFromArray(const std::string& doc,
                                         const std::string& field_path,
                                         const std::vector<std::string>& vals) const override;

            std::string PopArray(const std::string& doc,
                                 const std::string& field_path,
                                 bool               from_end) const override;

            std::string Project(const std::string&              doc,
                                const std::vector<std::string>& fields,
                                bool                            exclude) const override;

            std::string RenameField(const std::string& doc,
                                    const std::string& old_field,
                                    const std::string& new_field) const override;

        private:
            // متدهای کمکی داخلی
            static std::string  EncodeValue(const std::string& val, UpdateValueType vt);
            static FieldValue   DetectType(const std::string& raw_val);
            static bool         CompareValues(const FieldValue& a,
                                              const std::string& b_raw,
                                              ValueType          b_type,
                                              Op                 op);
        };

// ══════════════════════════════════════════════════════════════
// §4  کلاس اصلی Evaluator
// ══════════════════════════════════════════════════════════════

/**
 * @class Evaluator
 * @brief ارزیاب شروط و اعمال‌کننده Update روی اسناد
 *
 * @details
 * این کلاس stateless است (همه متدها const یا static) —
 * یک instance می‌تواند در چند thread هم‌زمان استفاده شود.
 *
 * @section usage استفاده
 *
 * **در DocEngine:**
 * ```cpp
 * // DocEngine یک instance نگه می‌دارد
 * nexora::query::Evaluator evaluator_;
 *
 * // در FindMany:
 * if (evaluator_.Match(bson, condition)) { ... }
 *
 * // در UpdateById:
 * std::string new_bson = evaluator_.Apply(old_bson, update_spec);
 * ```
 *
 * **در GraphEngine:**
 * ```cpp
 * // اگر نیاز به فیلتر داشت:
 * nexora::query::Evaluator eval;
 * Condition is_active = Condition::Leaf("active", Op::EQ, "1", ValueType::Bool);
 * doc_engine.IterateCollection("users",
 *     [&](const std::string& id, const std::string& bson) -> bool {
 *         if (eval.Match(bson, is_active)) {
 *             graph.AddNode(id, bson);
 *         }
 *         return true;
 *     });
 * ```
 *
 * @note
 * در MVP از JsonAdapter استفاده می‌شود.
 * برای تغییر به LibBsonAdapter: `Evaluator eval(std::make_unique<LibBsonAdapter>())`
 */
        class Evaluator {
        public:
            /**
             * @brief سازنده با آداپتور پیش‌فرض (JsonAdapter)
             */
            Evaluator();

            /**
             * @brief سازنده با آداپتور سفارشی (برای تست یا LibBson)
             */
            explicit Evaluator(std::unique_ptr<IBsonAdapter> adapter);

            // ──────────────────────────────────────────────────────────
            // 4.1  تطابق شرط با سند
            // ──────────────────────────────────────────────────────────

            /**
             * @brief بررسی می‌کند آیا سند با شرط تطابق دارد
             * @param bson_doc  محتوای سند
             * @param condition شرط
             * @return true اگر تطابق دارد
             *
             * @details
             * - شرط خالی (`IsEmpty()`) همیشه true برمی‌گرداند
             * - شرط ترکیبی (AND/OR/NOR) را به صورت recursive ارزیابی می‌کند
             * - شرط ساده (leaf) مقدار فیلد را با op مقایسه می‌کند
             *
             * @example
             * ```cpp
             * Evaluator eval;
             *
             * // شرط ساده
             * auto cond = Condition::Leaf("age", Op::GT, "18", ValueType::Int64);
             * bool ok = eval.Match(doc, cond);
             *
             * // شرط ترکیبی
             * auto compound = Condition::And({
             *     Condition::Leaf("age",    Op::GTE, "18", ValueType::Int64),
             *     Condition::Leaf("active", Op::EQ,  "1",  ValueType::Bool)
             * });
             * bool ok2 = eval.Match(doc, compound);
             * ```
             */
            bool Match(const std::string& bson_doc,
                       const Condition&   condition) const;

            // ──────────────────────────────────────────────────────────
            // 4.2  اعمال Update روی سند
            // ──────────────────────────────────────────────────────────

            /**
             * @brief UpdateSpec را روی یک سند اعمال می‌کند و سند جدید برمی‌گرداند
             * @param bson_doc    سند اصلی
             * @param update_spec عملیات به‌روزرسانی
             * @return سند به‌روز شده
             *
             * @details
             * سند اصلی **تغییر نمی‌کند** — یک کپی جدید برمی‌گرداند.
             * عملیات‌ها به ترتیب اعمال می‌شوند.
             *
             * @example
             * ```cpp
             * UpdateSpec spec;
             * spec.Set("bio", "Hello World")
             *     .Inc("likes", "1", UpdateValueType::Int64)
             *     .TouchDate("updated_at");
             *
             * std::string new_doc = eval.Apply(old_doc, spec);
             * ```
             */
            std::string Apply(const std::string& bson_doc,
                              const UpdateSpec&  update_spec) const;

            // ──────────────────────────────────────────────────────────
            // 4.3  Projection
            // ──────────────────────────────────────────────────────────

            /**
             * @brief فیلدهای خروجی را محدود می‌کند
             * @param bson_doc  سند
             * @param opts      QueryOptions شامل projection
             * @return سند با فقط فیلدهای مورد نظر
             *
             * @details
             * - اگر `opts.projection` خالی باشد: سند بدون تغییر برمی‌گردد
             * - `projection_exclude=false` (include mode): فقط این فیلدها + "_id" برگردند
             * - `projection_exclude=true` (exclude mode): این فیلدها حذف شوند
             *
             * @example
             * ```cpp
             * QueryOptions opts;
             * opts.projection = {"username", "email"};  // فقط این دو فیلد
             * std::string projected = eval.ApplyProjection(doc, opts);
             * ```
             */
            std::string ApplyProjection(const std::string&  bson_doc,
                                        const QueryOptions& opts) const;

            // ──────────────────────────────────────────────────────────
            // 4.4  استخراج مقدار فیلد (public utility)
            // ──────────────────────────────────────────────────────────

            /**
             * @brief یک فیلد را از سند استخراج می‌کند
             * @param bson_doc   سند
             * @param field_path مسیر فیلد (با . برای nested: "address.city")
             * @return FieldValue با مقدار و نوع
             *
             * @note
             * تیم GraphEngine از این برای خواندن فیلدهای خاص استفاده می‌کند:
             * ```cpp
             * auto author = eval.ExtractField(bson, "author_id");
             * if (author.found) graph.AddEdge(author.raw, post_id);
             * ```
             */
            FieldValue ExtractField(const std::string& bson_doc,
                                    const std::string& field_path) const;

        private:
            std::unique_ptr<IBsonAdapter> adapter_;

            // متدهای کمکی داخلی
            bool MatchLeaf(const std::string& bson_doc,
                           const Condition&   condition) const;

            bool MatchComposite(const std::string& bson_doc,
                                const Condition&   condition) const;

            std::string ApplyOperation(const std::string&       bson_doc,
                                       const UpdateOperation&   op) const;
        };

    } // namespace query
} // namespace nexora