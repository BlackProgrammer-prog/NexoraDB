//
// Created by HOME on 6/5/2026.
//

#ifndef NEXORADB_CONDITION_H
#define NEXORADB_CONDITION_H

#endif //NEXORADB_CONDITION_H


#pragma once

/**
 * @file query/Condition.h
 * @brief تعریف ساختارهای فیلتر و شرط برای Query Layer
 *
 * @details
 * این فایل تمام انواع شرط‌های جستجو را تعریف می‌کند.
 * تیم parser این struct‌ها را از کوئری ورودی (dict پایتون یا DSL) می‌سازد
 * و به DocEngine/GraphEngine پاس می‌دهد.
 *
 * @section flow جریان داده
 * ```
 * Python dict / DSL string
 *       ↓  (Cython parser)
 *  Condition struct
 *       ↓
 *  DocEngine::FindMany / UpdateMany / DeleteMany
 *       ↓
 *  Evaluator::Match(bson, condition)
 * ```
 *
 * @note
 * هیچ JSON‌ای به C++ منتقل نمی‌شود.
 * تمام مقادیر از طریق تایپ‌های پایه / enum به این struct‌ها می‌رسند.
 */

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nexora {
    namespace query {

// ══════════════════════════════════════════════════════════════
// §1  عملگرهای مقایسه‌ای
// ══════════════════════════════════════════════════════════════

/**
 * @enum Op
 * @brief عملگرهای مقایسه‌ای برای فیلتر روی فیلد
 *
 * | عملگر  | معادل MongoDB | توضیح                          |
 * |--------|--------------|-------------------------------|
 * | EQ     | $eq          | برابر                         |
 * | NEQ    | $ne          | نابرابر                       |
 * | GT     | $gt          | بزرگ‌تر                       |
 * | GTE    | $gte         | بزرگ‌تر یا مساوی              |
 * | LT     | $lt          | کوچک‌تر                       |
 * | LTE    | $lte         | کوچک‌تر یا مساوی             |
 * | IN     | $in          | عضویت در لیست                |
 * | NIN    | $nin         | عدم عضویت در لیست            |
 * | EXISTS | $exists      | وجود/عدم وجود فیلد           |
 * | REGEX  | $regex       | تطابق با الگوی regex         |
 * | STARTS | -            | شروع با رشته (for string idx) |
 * | CONTAINS | -          | حاوی رشته (full scan)        |
 */
        enum class Op : uint8_t {
            EQ       = 0,
            NEQ      = 1,
            GT       = 2,
            GTE      = 3,
            LT       = 4,
            LTE      = 5,
            IN       = 6,   ///< مقدار value = مقادیر با \0 جدا شده
            NIN      = 7,   ///< مقدار value = مقادیر با \0 جدا شده
            EXISTS   = 8,   ///< value = "1" یعنی باید وجود داشته باشد، "0" یعنی نباشد
            REGEX    = 9,   ///< value = الگوی regex (POSIX extended)
            STARTS   = 10,  ///< value = prefix مورد انتظار
            CONTAINS = 11   ///< value = substring مورد انتظار
        };

// ══════════════════════════════════════════════════════════════
// §2  نوع مقدار در شرط
// ══════════════════════════════════════════════════════════════

/**
 * @enum ValueType
 * @brief نوع تایپ مقدار در Condition
 *
 * @details
 * Evaluator از این برای cast درست استفاده می‌کند:
 * مقایسه عددی vs رشته‌ای vs بولین فرق دارد.
 */
        enum class ValueType : uint8_t {
            String  = 0,
            Int64   = 1,
            Float64 = 2,
            Bool    = 3,
            Null    = 4
        };

// ══════════════════════════════════════════════════════════════
// §3  نوع ترکیب شروط
// ══════════════════════════════════════════════════════════════

/**
 * @enum LogicOp
 * @brief عملگر منطقی برای ترکیب چند Condition
 */
        enum class LogicOp : uint8_t {
            AND = 0,  ///< همه شروط باید صادق باشند
            OR  = 1,  ///< حداقل یک شرط کافی است
            NOR = 2,  ///< هیچ‌کدام نباید صادق باشند
            NOT = 3   ///< فقط یک sub-condition را نقیض می‌کند
        };

// ══════════════════════════════════════════════════════════════
// §4  ساختار اصلی Condition
// ══════════════════════════════════════════════════════════════

/**
 * @struct Condition
 * @brief یک شرط اتمیک یا ترکیبی برای فیلتر اسناد
 *
 * @details
 * دو نوع استفاده دارد:
 *
 * **الف) شرط ساده (leaf node):**
 * ```
 * field != ""  →  یک مقایسه ساده روی یک فیلد
 * ```
 *
 * **ب) شرط ترکیبی (composite node):**
 * ```
 * sub_conditions != empty  →  ترکیب AND/OR/NOR چند شرط
 * ```
 *
 * @example شرط ساده
 * ```cpp
 * Condition c;
 * c.field      = "age";
 * c.op         = Op::GT;
 * c.value      = "25";
 * c.value_type = ValueType::Int64;
 * ```
 *
 * @example شرط AND ترکیبی
 * ```cpp
 * Condition c;
 * c.logic = LogicOp::AND;
 * c.sub_conditions = {
 *     MakeLeaf("age",    Op::GTE, "18", ValueType::Int64),
 *     MakeLeaf("active", Op::EQ,  "1",  ValueType::Bool)
 * };
 * ```
 *
 * @example شرط IN
 * ```cpp
 * Condition c;
 * c.field  = "status";
 * c.op     = Op::IN;
 * c.values = {"active", "pending", "verified"};
 * // یا: c.value = "active\0pending\0verified"
 * ```
 *
 * @note برای parser تیم:
 *   - `IsLeaf()` → true اگر شرط ساده باشد
 *   - `IsEmpty()` → true اگر هیچ شرطی تعریف نشده باشد (match همه)
 */
        struct Condition {
            // ── شرط ساده (leaf) ──
            std::string field;                     ///< نام فیلد (مثلاً "age" یا "address.city")
            Op          op         = Op::EQ;       ///< عملگر مقایسه
            std::string value;                     ///< مقدار مقایسه (رشته‌ای encode شده)
            ValueType   value_type = ValueType::String; ///< نوع اصلی مقدار برای cast درست

            /// برای IN / NIN — لیست مقادیر (جایگزین بهتر از encode در value)
            std::vector<std::string> values;

            // ── شرط ترکیبی (composite) ──
            LogicOp                  logic = LogicOp::AND; ///< عملگر ترکیب
            std::vector<Condition>   sub_conditions;        ///< زیر-شروط

            // ── متدهای کمکی ──

            /**
             * @brief آیا این یک شرط ساده (leaf) است؟
             * @return true اگر field دارد و sub_conditions ندارد
             */
            bool IsLeaf() const noexcept {
                return !field.empty() && sub_conditions.empty();
            }

            /**
             * @brief آیا این شرط خالی است؟ (match همه اسناد)
             * @return true اگر نه field دارد و نه sub_conditions
             */
            bool IsEmpty() const noexcept {
                return field.empty() && sub_conditions.empty();
            }

            /**
             * @brief آیا این یک شرط ترکیبی است؟
             */
            bool IsComposite() const noexcept {
                return !sub_conditions.empty();
            }

            // ── Factory methods (برای راحتی ساخت در تست و Cython) ──

            /**
             * @brief ساخت یک شرط ساده
             *
             * @example
             * ```cpp
             * auto c = Condition::Leaf("age", Op::GT, "18", ValueType::Int64);
             * ```
             */
            static Condition Leaf(const std::string& field,
                                  Op                 op,
                                  const std::string& value,
                                  ValueType          vt = ValueType::String) {
                Condition c;
                c.field      = field;
                c.op         = op;
                c.value      = value;
                c.value_type = vt;
                return c;
            }

            /**
             * @brief ساخت یک شرط IN
             *
             * @example
             * ```cpp
             * auto c = Condition::In("status", {"active", "verified"});
             * ```
             */
            static Condition In(const std::string&              field,
                                std::vector<std::string>        vals,
                                bool                            negate = false) {
                Condition c;
                c.field  = field;
                c.op     = negate ? Op::NIN : Op::IN;
                c.values = std::move(vals);
                return c;
            }

            /**
             * @brief ساخت AND از چند شرط
             *
             * @example
             * ```cpp
             * auto c = Condition::And({
             *     Condition::Leaf("age",    Op::GTE, "18", ValueType::Int64),
             *     Condition::Leaf("active", Op::EQ,  "1",  ValueType::Bool)
             * });
             * ```
             */
            static Condition And(std::vector<Condition> subs) {
                Condition c;
                c.logic          = LogicOp::AND;
                c.sub_conditions = std::move(subs);
                return c;
            }

            /**
             * @brief ساخت OR از چند شرط
             */
            static Condition Or(std::vector<Condition> subs) {
                Condition c;
                c.logic          = LogicOp::OR;
                c.sub_conditions = std::move(subs);
                return c;
            }

            /**
             * @brief ساخت NOR از چند شرط
             */
            static Condition Nor(std::vector<Condition> subs) {
                Condition c;
                c.logic          = LogicOp::NOR;
                c.sub_conditions = std::move(subs);
                return c;
            }
        };

// ══════════════════════════════════════════════════════════════
// §5  ساختار Sort
// ══════════════════════════════════════════════════════════════

/**
 * @enum SortOrder
 * @brief جهت مرتب‌سازی
 */
        enum class SortOrder : uint8_t {
            Ascending  = 0,
            Descending = 1
        };

/**
 * @struct SortField
 * @brief یک فیلد در مشخصات مرتب‌سازی
 */
        struct SortField {
            std::string field;
            SortOrder   order = SortOrder::Ascending;
        };

/**
 * @struct SortSpec
 * @brief مشخصات کامل مرتب‌سازی نتایج
 *
 * @example
 * ```cpp
 * SortSpec sort;
 * sort.fields = {
 *     {"created_at", SortOrder::Descending},
 *     {"username",   SortOrder::Ascending}
 * };
 * ```
 */
        struct SortSpec {
            std::vector<SortField> fields;

            bool IsEmpty() const noexcept { return fields.empty(); }
        };

// ══════════════════════════════════════════════════════════════
// §6  ساختار QueryOptions (ترکیب limit + skip + sort)
// ══════════════════════════════════════════════════════════════

/**
 * @struct QueryOptions
 * @brief گزینه‌های اضافی یک کوئری Find
 *
 * @details
 * این struct به FindMany پاس داده می‌شود تا pagination و sorting را کنترل کند.
 * در DocEngine فعلی (MVP) sorting در لایه C++ پیاده‌سازی نشده —
 * Cython layer باید نتایج را sort کند یا در نسخه بعدی DocEngine را extend کند.
 *
 * @example
 * ```cpp
 * QueryOptions opts;
 * opts.limit = 20;
 * opts.skip  = 40;   // صفحه سوم
 * opts.sort.fields = {{"created_at", SortOrder::Descending}};
 * ```
 */
        struct QueryOptions {
            uint32_t limit = 0;   ///< 0 = بدون محدودیت
            uint32_t skip  = 0;   ///< برای pagination
            SortSpec sort;        ///< مشخصات مرتب‌سازی (اختیاری)

            /// projection: فقط این فیلدها برگردانده شوند (خالی = همه)
            std::vector<std::string> projection;

            /// اگر true: فیلدهای projection حذف می‌شوند (exclude mode)
            bool projection_exclude = false;
        };

    } // namespace query
} // namespace nexora