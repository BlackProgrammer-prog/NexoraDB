//
// Created by HOME on 6/5/2026.
//

#ifndef NEXORADB_UPDATESPEC_H
#define NEXORADB_UPDATESPEC_H

#endif //NEXORADB_UPDATESPEC_H

#pragma once

/**
 * @file query/UpdateSpec.h
 * @brief ساختارهای به‌روزرسانی سند برای Query Layer
 *
 * @details
 * این فایل تمام عملیات ممکن برای به‌روزرسانی اسناد را تعریف می‌کند.
 * تیم parser این struct‌ها را از کوئری ورودی می‌سازد
 * و DocEngine آن‌ها را اجرا می‌کند.
 *
 * @section operators عملگرهای پشتیبانی شده
 * ```
 * $set    → UpdateOp::Set     تنظیم/ایجاد فیلد
 * $unset  → UpdateOp::Unset   حذف فیلد
 * $inc    → UpdateOp::Inc     افزودن عدد (منفی هم قبول می‌شود)
 * $mul    → UpdateOp::Mul     ضرب مقدار عددی
 * $min    → UpdateOp::Min     اگر مقدار جدید کمتر است جایگزین کن
 * $max    → UpdateOp::Max     اگر مقدار جدید بیشتر است جایگزین کن
 * $push   → UpdateOp::Push    افزودن عنصر به آرایه
 * $pull   → UpdateOp::Pull    حذف عنصر از آرایه (با شرط)
 * $addToSet → UpdateOp::AddToSet  افزودن به آرایه اگر نباشد
 * $pop    → UpdateOp::Pop     حذف اول/آخر آرایه
 * $rename → UpdateOp::Rename  تغییر نام فیلد
 * $currentDate → UpdateOp::CurrentDate  تنظیم timestamp فعلی
 * ```
 */

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nexora {
    namespace query {

// ══════════════════════════════════════════════════════════════
// §1  انواع عملیات Update
// ══════════════════════════════════════════════════════════════

/**
 * @enum UpdateOp
 * @brief نوع عملیات به‌روزرسانی روی یک فیلد
 */
        enum class UpdateOp : uint8_t {
            // ── فیلد scalar ──
            Set         = 0,  ///< تنظیم مقدار (ایجاد اگر وجود ندارد)
            Unset       = 1,  ///< حذف فیلد از سند
            Inc         = 2,  ///< val += delta (عدد، منفی مجاز)
            Mul         = 3,  ///< val *= factor
            Min         = 4,  ///< val = min(val, new_val)
            Max         = 5,  ///< val = max(val, new_val)
            Rename      = 6,  ///< تغییر نام فیلد (value = نام جدید)
            CurrentDate = 7,  ///< تنظیم Unix timestamp فعلی (milliseconds)

            // ── فیلد array ──
            Push        = 8,  ///< افزودن یک عنصر به انتهای آرایه
            PushAll     = 9,  ///< افزودن چند عنصر (values)
            Pull        = 10, ///< حذف همه عناصر برابر با value
            PullAll     = 11, ///< حذف همه عناصر موجود در values
            AddToSet    = 12, ///< افزودن فقط اگر عنصر در آرایه نباشد
            Pop         = 13, ///< value="1" → حذف آخر، value="-1" → حذف اول
        };

// ══════════════════════════════════════════════════════════════
// §2  نوع مقدار در UpdateOperation
// ══════════════════════════════════════════════════════════════

/**
 * @enum UpdateValueType
 * @brief نوع تایپ مقدار جدید
 *
 * @details
 * Evaluator از این برای cast درست در BSON استفاده می‌کند.
 * مثلاً Inc باید عدد را به عدد اضافه کند، نه string concatenation.
 */
        enum class UpdateValueType : uint8_t {
            String  = 0,
            Int64   = 1,
            Float64 = 2,
            Bool    = 3,
            Null    = 4,
            Array   = 5  ///< برای PushAll/PullAll — در values لیست است
        };

// ══════════════════════════════════════════════════════════════
// §3  یک عملیات تکی
// ══════════════════════════════════════════════════════════════

/**
 * @struct UpdateOperation
 * @brief یک عملیات به‌روزرسانی روی یک فیلد مشخص
 *
 * @details
 * هر عملیات شامل:
 *   - نوع عملیات (op)
 *   - نام فیلد هدف (field) — می‌تواند nested باشد: "address.city"
 *   - مقدار جدید (value یا values برای array ops)
 *   - نوع تایپ (value_type) برای cast صحیح
 *
 * @example Set
 * ```cpp
 * UpdateOperation op;
 * op.op         = UpdateOp::Set;
 * op.field      = "bio";
 * op.value      = "Hello World";
 * op.value_type = UpdateValueType::String;
 * ```
 *
 * @example Inc
 * ```cpp
 * UpdateOperation op;
 * op.op         = UpdateOp::Inc;
 * op.field      = "likes";
 * op.value      = "1";         // delta
 * op.value_type = UpdateValueType::Int64;
 * ```
 *
 * @example Push به آرایه
 * ```cpp
 * UpdateOperation op;
 * op.op         = UpdateOp::Push;
 * op.field      = "tags";
 * op.value      = "sports";
 * op.value_type = UpdateValueType::String;
 * ```
 *
 * @example PushAll
 * ```cpp
 * UpdateOperation op;
 * op.op         = UpdateOp::PushAll;
 * op.field      = "tags";
 * op.values     = {"sports", "health", "outdoor"};
 * op.value_type = UpdateValueType::Array;
 * ```
 */
        struct UpdateOperation {
            UpdateOp          op         = UpdateOp::Set;
            std::string       field;              ///< نام فیلد (از جمله nested: "a.b.c")
            std::string       value;             ///< مقدار برای Set/Inc/Mul/Min/Max/Push/Pull/Pop/Rename
            std::vector<std::string> values;     ///< مقادیر برای PushAll/PullAll/AddToSet
            UpdateValueType   value_type = UpdateValueType::String; ///< نوع تایپ برای cast

            // ── Factory methods ──

            /**
             * @brief Set یک فیلد
             * @example `UpdateOperation::MakeSet("username", "alice")`
             */
            static UpdateOperation MakeSet(const std::string& field,
                                           const std::string& value,
                                           UpdateValueType    vt = UpdateValueType::String) {
                return {UpdateOp::Set, field, value, {}, vt};
            }

            /**
             * @brief Unset (حذف) یک فیلد
             */
            static UpdateOperation MakeUnset(const std::string& field) {
                return {UpdateOp::Unset, field, "", {}, UpdateValueType::Null};
            }

            /**
             * @brief افزودن delta به یک فیلد عددی
             * @param delta عدد (می‌تواند منفی باشد برای کاهش)
             */
            static UpdateOperation MakeInc(const std::string& field,
                                           const std::string& delta,
                                           UpdateValueType    vt = UpdateValueType::Int64) {
                return {UpdateOp::Inc, field, delta, {}, vt};
            }

            /**
             * @brief Push یک عنصر به آرایه
             */
            static UpdateOperation MakePush(const std::string& field,
                                            const std::string& element,
                                            UpdateValueType    vt = UpdateValueType::String) {
                return {UpdateOp::Push, field, element, {}, vt};
            }

            /**
             * @brief Pull عناصر برابر با value از آرایه
             */
            static UpdateOperation MakePull(const std::string& field,
                                            const std::string& element,
                                            UpdateValueType    vt = UpdateValueType::String) {
                return {UpdateOp::Pull, field, element, {}, vt};
            }

            /**
             * @brief CurrentDate — تنظیم timestamp فعلی (Unix ms)
             */
            static UpdateOperation MakeCurrentDate(const std::string& field) {
                return {UpdateOp::CurrentDate, field, "", {}, UpdateValueType::Int64};
            }
        };

// ══════════════════════════════════════════════════════════════
// §4  مجموعه عملیات (UpdateSpec)
// ══════════════════════════════════════════════════════════════

/**
 * @struct UpdateSpec
 * @brief مجموعه‌ای از عملیات به‌روزرسانی که روی یک سند اعمال می‌شود
 *
 * @details
 * عملیات‌ها **به ترتیب** اعمال می‌شوند.
 * این یعنی اگر بنویسید:
 *   1. Set("likes", "0")
 *   2. Inc("likes", "5")
 * نتیجه likes=5 خواهد بود.
 *
 * @example
 * ```cpp
 * UpdateSpec spec;
 * spec.Add(UpdateOperation::MakeSet("bio",    "Hello"));
 * spec.Add(UpdateOperation::MakeInc("likes",  "1", UpdateValueType::Int64));
 * spec.Add(UpdateOperation::MakeCurrentDate("updated_at"));
 * engine.UpdateById("posts", post_id, spec);
 * ```
 *
 * @note
 * برای upsert (insert if not exists):
 * فیلد `upsert = true` را تنظیم کنید —
 * DocEngine اگر سند پیدا نشد با UpdateSpec یک سند جدید می‌سازد.
 * (در MVP پیاده‌سازی نشده — TODO)
 */
        struct UpdateSpec {
            std::vector<UpdateOperation> operations;

            /**
             * @brief اگر true و سند پیدا نشود، یک سند جدید درج می‌شود
             * @note در MVP پیاده‌سازی نشده
             */
            bool upsert = false;

            /**
             * @brief افزودن یک عملیات به spec
             */
            UpdateSpec& Add(UpdateOperation op) {
                operations.push_back(std::move(op));
                return *this;
            }

            /**
             * @brief آیا spec خالی است؟
             */
            bool IsEmpty() const noexcept { return operations.empty(); }

            // ── Builder shortcuts ──

            UpdateSpec& Set(const std::string& field, const std::string& val,
                            UpdateValueType vt = UpdateValueType::String) {
                return Add(UpdateOperation::MakeSet(field, val, vt));
            }

            UpdateSpec& Unset(const std::string& field) {
                return Add(UpdateOperation::MakeUnset(field));
            }

            UpdateSpec& Inc(const std::string& field, const std::string& delta,
                            UpdateValueType vt = UpdateValueType::Int64) {
                return Add(UpdateOperation::MakeInc(field, delta, vt));
            }

            UpdateSpec& Push(const std::string& field, const std::string& element,
                             UpdateValueType vt = UpdateValueType::String) {
                return Add(UpdateOperation::MakePush(field, element, vt));
            }

            UpdateSpec& Pull(const std::string& field, const std::string& element,
                             UpdateValueType vt = UpdateValueType::String) {
                return Add(UpdateOperation::MakePull(field, element, vt));
            }

            UpdateSpec& TouchDate(const std::string& field) {
                return Add(UpdateOperation::MakeCurrentDate(field));
            }
        };

    } // namespace query
} // namespace nexora