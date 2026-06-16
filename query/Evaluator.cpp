//
// Created by HOME on 6/5/2026.
//

/**
 * @file query/Evaluator.cpp
 * @brief پیاده‌سازی Evaluator و JsonAdapter
 */

#include "Evaluator.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace nexora {
    namespace query {

// ══════════════════════════════════════════════════════════════
// §1  ابزارهای رشته‌ای داخلی
// ══════════════════════════════════════════════════════════════

        namespace detail {

/// حذف whitespace از ابتدا و انتها
            static std::string Trim(const std::string& s) {
                const char* ws = " \t\r\n";
                size_t start = s.find_first_not_of(ws);
                if (start == std::string::npos) return "";
                size_t end = s.find_last_not_of(ws);
                return s.substr(start, end - start + 1);
            }

/**
 * @brief استخراج مقدار یک فیلد از JSON ساده
 * @details از nested path با . پشتیبانی می‌کند: "address.city"
 * @return {raw_value, found}
 */
            static std::pair<std::string, bool>
            ExtractJsonField(const std::string& json, const std::string& field_path) {
                // تفکیک nested path
                auto dot = field_path.find('.');
                std::string top = (dot == std::string::npos) ? field_path : field_path.substr(0, dot);
                std::string rest = (dot == std::string::npos) ? "" : field_path.substr(dot + 1);

                // جستجوی کلید در JSON
                std::string search = "\"" + top + "\"";
                size_t pos = json.find(search);
                if (pos == std::string::npos) return {"", false};

                // پیدا کردن ':'
                size_t colon = json.find(':', pos + search.size());
                if (colon == std::string::npos) return {"", false};
                ++colon;

                // رد کردن whitespace بعد از ':'
                while (colon < json.size() && (json[colon] == ' ' || json[colon] == '\t')) ++colon;
                if (colon >= json.size()) return {"", false};

                char first = json[colon];

                if (first == '{') {
                    // مقدار آبجکت — برای nested
                    int depth = 0;
                    size_t start = colon;
                    for (size_t i = colon; i < json.size(); ++i) {
                        if (json[i] == '{') ++depth;
                        else if (json[i] == '}') {
                            --depth;
                            if (depth == 0) {
                                std::string sub_obj = json.substr(start, i - start + 1);
                                if (!rest.empty()) return ExtractJsonField(sub_obj, rest);
                                return {sub_obj, true};
                            }
                        }
                    }
                    return {"", false};
                }

                if (first == '[') {
                    // مقدار آرایه
                    int depth = 0;
                    size_t start = colon;
                    for (size_t i = colon; i < json.size(); ++i) {
                        if (json[i] == '[') ++depth;
                        else if (json[i] == ']') {
                            --depth;
                            if (depth == 0) return {json.substr(start, i - start + 1), true};
                        }
                    }
                    return {"", false};
                }

                if (first == '"') {
                    // مقدار رشته
                    size_t start = colon + 1;
                    size_t end = start;
                    while (end < json.size()) {
                        if (json[end] == '\\') { end += 2; continue; }
                        if (json[end] == '"') break;
                        ++end;
                    }
                    return {json.substr(start, end - start), true};
                }

                if (first == 'n' && json.substr(colon, 4) == "null") {
                    return {"null", true};
                }

                // مقدار عددی یا boolean
                size_t end = json.find_first_of(",}\n", colon);
                if (end == std::string::npos) end = json.size();
                std::string val = Trim(json.substr(colon, end - colon));
                return {val, true};
            }

/**
 * @brief تشخیص نوع مقدار از رشته
 */
            static ValueType DetectValueType(const std::string& raw) {
                if (raw.empty() || raw == "null") return ValueType::Null;
                if (raw == "true" || raw == "false") return ValueType::Bool;

                // بررسی عدد صحیح
                bool is_int = true;
                size_t start = 0;
                if (!raw.empty() && raw[0] == '-') start = 1;
                for (size_t i = start; i < raw.size(); ++i) {
                    if (!std::isdigit(raw[i])) { is_int = false; break; }
                }
                if (is_int && start < raw.size()) return ValueType::Int64;

                // بررسی عدد اعشاری
                try {
                    std::stod(raw);
                    return ValueType::Float64;
                } catch (...) {}

                return ValueType::String;
            }

/**
 * @brief مقایسه دو مقدار با op
 * @param field_raw  مقدار فیلد از سند
 * @param field_type نوع فیلد
 * @param cond_raw   مقدار شرط
 * @param cond_type  نوع شرط
 * @param op         عملگر
 */
            static bool CompareScalar(const std::string& field_raw, ValueType field_type,
                                      const std::string& cond_raw,  ValueType cond_type,
                                      Op op) {
                // مقایسه عددی
                bool field_is_num = (field_type == ValueType::Int64 || field_type == ValueType::Float64);
                bool cond_is_num  = (cond_type  == ValueType::Int64 || cond_type  == ValueType::Float64);

                if (field_is_num && cond_is_num) {
                    double fv = 0, cv = 0;
                    try { fv = std::stod(field_raw); } catch (...) {}
                    try { cv = std::stod(cond_raw);  } catch (...) {}
                    switch (op) {
                        case Op::EQ:  return std::abs(fv - cv) < 1e-12;
                        case Op::NEQ: return std::abs(fv - cv) >= 1e-12;
                        case Op::GT:  return fv > cv;
                        case Op::GTE: return fv >= cv;
                        case Op::LT:  return fv < cv;
                        case Op::LTE: return fv <= cv;
                        default: break;
                    }
                }

                // مقایسه boolean
                if (field_type == ValueType::Bool || cond_type == ValueType::Bool) {
                    bool fv = (field_raw == "true" || field_raw == "1");
                    bool cv = (cond_raw  == "true" || cond_raw  == "1");
                    switch (op) {
                        case Op::EQ:  return fv == cv;
                        case Op::NEQ: return fv != cv;
                        default: return false;
                    }
                }

                // مقایسه رشته‌ای
                switch (op) {
                    case Op::EQ:       return field_raw == cond_raw;
                    case Op::NEQ:      return field_raw != cond_raw;
                    case Op::GT:       return field_raw >  cond_raw;
                    case Op::GTE:      return field_raw >= cond_raw;
                    case Op::LT:       return field_raw <  cond_raw;
                    case Op::LTE:      return field_raw <= cond_raw;
                    case Op::STARTS:   return field_raw.find(cond_raw) == 0;
                    case Op::CONTAINS: return field_raw.find(cond_raw) != std::string::npos;
                    case Op::REGEX: {
                        try {
                            std::regex re(cond_raw, std::regex::extended);
                            return std::regex_search(field_raw, re);
                        } catch (...) { return false; }
                    }
                    default: return false;
                }
            }

/**
 * @brief تبدیل مقدار + نوع به فرمت JSON-like برای ذخیره در BSON
 */
            static std::string EncodeAsJson(const std::string& val, UpdateValueType vt) {
                switch (vt) {
                    case UpdateValueType::String:  return "\"" + val + "\"";
                    case UpdateValueType::Int64:
                    case UpdateValueType::Float64: return val;
                    case UpdateValueType::Bool:    return (val == "true" || val == "1") ? "true" : "false";
                    case UpdateValueType::Null:    return "null";
                    case UpdateValueType::Array:   return val; // از پیش encode شده
                    default:                       return "\"" + val + "\"";
                }
            }

        } // namespace detail

// ══════════════════════════════════════════════════════════════
// §2  پیاده‌سازی JsonAdapter
// ══════════════════════════════════════════════════════════════

        FieldValue JsonAdapter::GetField(const std::string& doc,
                                         const std::string& field_path) const {
            auto [raw, found] = detail::ExtractJsonField(doc, field_path);
            if (!found) return {"", ValueType::Null, false};
            ValueType vt = detail::DetectValueType(raw);
            return {raw, vt, true};
        }

        std::string JsonAdapter::SetField(const std::string& doc,
                                          const std::string& field_path,
                                          const std::string& value,
                                          UpdateValueType    vt) const {
            // فقط top-level فیلدها در MVP
            // nested در نسخه بعدی
            auto dot = field_path.find('.');
            if (dot != std::string::npos) {
                // TODO: nested field set
                return doc;
            }

            std::string encoded = detail::EncodeAsJson(value, vt);
            std::string search  = "\"" + field_path + "\":";
            std::string result  = doc;

            auto pos = result.find(search);
            if (pos != std::string::npos) {
                // پیدا کردن محدوده مقدار قدیمی
                size_t val_start = pos + search.size();
                while (val_start < result.size() &&
                       (result[val_start] == ' ' || result[val_start] == '\t')) ++val_start;

                size_t val_end = val_start;
                char first = result[val_start];

                if (first == '"') {
                    // رشته
                    val_end = val_start + 1;
                    while (val_end < result.size()) {
                        if (result[val_end] == '\\') { val_end += 2; continue; }
                        if (result[val_end] == '"') { ++val_end; break; }
                        ++val_end;
                    }
                } else if (first == '{' || first == '[') {
                    // آبجکت یا آرایه
                    char open = first, close = (first == '{') ? '}' : ']';
                    int depth = 0;
                    val_end = val_start;
                    while (val_end < result.size()) {
                        if (result[val_end] == open)  ++depth;
                        if (result[val_end] == close) { --depth; if (depth == 0) { ++val_end; break; } }
                        ++val_end;
                    }
                } else {
                    // عدد / bool / null
                    val_end = result.find_first_of(",}\n", val_start);
                    if (val_end == std::string::npos) val_end = result.size();
                }

                result.replace(val_start, val_end - val_start, encoded);
            } else {
                // اضافه کردن فیلد جدید
                auto close_pos = result.rfind('}');
                if (close_pos != std::string::npos) {
                    // آیا قبل از } محتوایی هست؟
                    std::string before = detail::Trim(result.substr(0, close_pos));
                    bool need_comma = !before.empty() && before.back() != '{';
                    std::string insert = std::string(need_comma ? "," : "") + "\"" + field_path + "\":" + encoded;

                    result.insert(close_pos, insert);
                }
            }
            return result;
        }

        std::string JsonAdapter::UnsetField(const std::string& doc,
                                            const std::string& field_path) const {
            std::string result = doc;
            // pattern: ,"field":value یا "field":value,
            std::string key_pattern = "\"" + field_path + "\":";
            auto pos = result.find(key_pattern);
            if (pos == std::string::npos) return result;

            // پیدا کردن محدوده کامل (کلید + مقدار)
            size_t key_start = pos;
            // بررسی کاما قبل
            if (key_start > 0 && result[key_start - 1] == ',') --key_start;
            else if (key_start > 1 && result[key_start - 2] == ',') --key_start;

            size_t val_start = pos + key_pattern.size();
            while (val_start < result.size() &&
                   (result[val_start] == ' ' || result[val_start] == '\t')) ++val_start;

            size_t val_end = val_start;
            char first = (val_start < result.size()) ? result[val_start] : 0;

            if (first == '"') {
                val_end = val_start + 1;
                while (val_end < result.size()) {
                    if (result[val_end] == '\\') { val_end += 2; continue; }
                    if (result[val_end] == '"') { ++val_end; break; }
                    ++val_end;
                }
            } else if (first == '{' || first == '[') {
                char open = first, close = (first == '{') ? '}' : ']';
                int depth = 0;
                val_end = val_start;
                while (val_end < result.size()) {
                    if (result[val_end] == open) ++depth;
                    if (result[val_end] == close) { --depth; if (!depth) { ++val_end; break; } }
                    ++val_end;
                }
            } else {
                val_end = result.find_first_of(",}\n", val_start);
                if (val_end == std::string::npos) val_end = result.size();
            }

            // حذف کاما بعد از مقدار اگر کاما قبل حذف نشد
            size_t remove_end = val_end;
            if (key_start == pos && remove_end < result.size() && result[remove_end] == ',') {
                ++remove_end;
            }

            result.erase(key_start, remove_end - key_start);
            return result;
        }

        std::string JsonAdapter::PushToArray(const std::string& doc,
                                             const std::string& field_path,
                                             const std::string& element,
                                             UpdateValueType    vt) const {
            auto [arr_raw, found] = detail::ExtractJsonField(doc, field_path);
            std::string encoded = detail::EncodeAsJson(element, vt);

            if (!found || arr_raw.empty() || arr_raw[0] != '[') {
                // آرایه وجود ندارد — ایجاد کن
                return SetField(doc, field_path, "[" + encoded + "]", UpdateValueType::Array);
            }

            // اضافه کردن به انتها
            std::string new_arr = arr_raw;
            auto close = new_arr.rfind(']');
            if (close == std::string::npos) return doc;

            std::string before = detail::Trim(new_arr.substr(1, close - 1));
            new_arr.insert(close, (before.empty() ? "" : ",") + encoded);
            return SetField(doc, field_path, new_arr, UpdateValueType::Array);
        }

        std::string JsonAdapter::PullFromArray(const std::string& doc,
                                               const std::string& field_path,
                                               const std::string& value) const {
            auto [arr_raw, found] = detail::ExtractJsonField(doc, field_path);
            if (!found || arr_raw.empty() || arr_raw[0] != '[') return doc;

            // پارس آرایه و حذف عناصر برابر با value
            std::string result = "[";
            bool first = true;
            // پارس ساده با split (در production از libbson استفاده کنید)
            std::string inner = arr_raw.substr(1, arr_raw.size() - 2);
            std::istringstream ss(inner);
            std::string token;
            while (std::getline(ss, token, ',')) {
                std::string t = detail::Trim(token);
                // حذف کوتیشن اگر رشته
                std::string bare = t;
                if (bare.size() >= 2 && bare.front() == '"' && bare.back() == '"')
                    bare = bare.substr(1, bare.size() - 2);
                if (bare == value || t == "\"" + value + "\"") continue;
                if (!first) result += ",";
                result += t;
                first = false;
            }
            result += "]";
            return SetField(doc, field_path, result, UpdateValueType::Array);
        }

        std::string JsonAdapter::PullAllFromArray(const std::string& doc,
                                                  const std::string& field_path,
                                                  const std::vector<std::string>& vals) const {
            std::string result = doc;
            for (const auto& v : vals) {
                result = PullFromArray(result, field_path, v);
            }
            return result;
        }

        std::string JsonAdapter::PopArray(const std::string& doc,
                                          const std::string& field_path,
                                          bool               from_end) const {
            auto [arr_raw, found] = detail::ExtractJsonField(doc, field_path);
            if (!found || arr_raw.size() < 2) return doc;

            std::string inner = arr_raw.substr(1, arr_raw.size() - 2);
            std::vector<std::string> elements;
            // پارس ساده
            std::istringstream ss(inner);
            std::string token;
            while (std::getline(ss, token, ',')) {
                std::string t = detail::Trim(token);
                if (!t.empty()) elements.push_back(t);
            }

            if (elements.empty()) return doc;
            if (from_end) elements.pop_back();
            else elements.erase(elements.begin());

            std::string new_arr = "[";
            for (size_t i = 0; i < elements.size(); ++i) {
                if (i > 0) new_arr += ",";
                new_arr += elements[i];
            }
            new_arr += "]";
            return SetField(doc, field_path, new_arr, UpdateValueType::Array);
        }

        std::string JsonAdapter::Project(const std::string&              doc,
                                         const std::vector<std::string>& fields,
                                         bool                            exclude) const {
            if (fields.empty()) return doc;

            if (exclude) {
                std::string result = doc;
                for (const auto& f : fields) result = UnsetField(result, f);
                return result;
            }

            // include mode: فقط فیلدهای مشخص + _id
            std::string result = "{";
            bool first = true;

            // همیشه _id را اضافه کن (مگر صراحتاً exclude شده باشد)
            auto id_val = GetField(doc, "_id");
            if (id_val.found) {
                result += "\"_id\":\"" + id_val.raw + "\"";
                first = false;
            }

            for (const auto& f : fields) {
                if (f == "_id") continue;
                auto val = GetField(doc, f);
                if (val.found) {
                    if (!first) result += ",";
                    result += "\"" + f + "\":";
                    if (val.type == ValueType::String)
                        result += "\"" + val.raw + "\"";
                    else
                        result += val.raw;
                    first = false;
                }
            }
            result += "}";
            return result;
        }

        std::string JsonAdapter::RenameField(const std::string& doc,
                                             const std::string& old_field,
                                             const std::string& new_field) const {
            auto [val, found] = detail::ExtractJsonField(doc, old_field);
            if (!found) return doc;

            ValueType vt = detail::DetectValueType(val);
            UpdateValueType uvt;
            switch (vt) {
                case ValueType::Int64:   uvt = UpdateValueType::Int64;   break;
                case ValueType::Float64: uvt = UpdateValueType::Float64; break;
                case ValueType::Bool:    uvt = UpdateValueType::Bool;    break;
                case ValueType::Null:    uvt = UpdateValueType::Null;    break;
                default:                 uvt = UpdateValueType::String;  break;
            }

            std::string without_old = UnsetField(doc, old_field);
            return SetField(without_old, new_field, val, uvt);
        }

// ══════════════════════════════════════════════════════════════
// §3  پیاده‌سازی Evaluator
// ══════════════════════════════════════════════════════════════

        Evaluator::Evaluator()
                : adapter_(std::make_unique<JsonAdapter>()) {}

        Evaluator::Evaluator(std::unique_ptr<IBsonAdapter> adapter)
                : adapter_(std::move(adapter)) {}

// ──────────────────────────────────────────────────────────────
// Match
// ──────────────────────────────────────────────────────────────

        bool Evaluator::Match(const std::string& bson_doc,
                              const Condition&   condition) const {
            if (condition.IsEmpty()) return true;
            if (condition.IsComposite()) return MatchComposite(bson_doc, condition);
            return MatchLeaf(bson_doc, condition);
        }

        bool Evaluator::MatchLeaf(const std::string& bson_doc,
                                  const Condition&   cond) const {
            FieldValue fv = adapter_->GetField(bson_doc, cond.field);

            // بررسی EXISTS
            if (cond.op == Op::EXISTS) {
                bool should_exist = (cond.value != "0" && cond.value != "false");
                return fv.found == should_exist;
            }

            if (!fv.found) return false;

            // بررسی IN / NIN
            const std::vector<std::string>* check_list = nullptr;
            std::vector<std::string> from_value;

            if (cond.op == Op::IN || cond.op == Op::NIN) {
                if (!cond.values.empty()) {
                    check_list = &cond.values;
                } else if (!cond.value.empty()) {
                    // decode از \0 separated
                    std::istringstream ss(cond.value);
                    std::string token;
                    while (std::getline(ss, token, '\0')) {
                        if (!token.empty()) from_value.push_back(token);
                    }
                    check_list = &from_value;
                }

                if (check_list) {
                    bool in_list = std::find(check_list->begin(), check_list->end(), fv.raw)
                                   != check_list->end();
                    return (cond.op == Op::IN) ? in_list : !in_list;
                }
                return false;
            }

            // مقایسه scalar
            ValueType cond_type = detail::DetectValueType(cond.value);
            // اگر caller نوع را مشخص کرده، از آن استفاده کن
            if (cond.value_type != ValueType::String) cond_type = cond.value_type;

            return detail::CompareScalar(fv.raw, fv.type, cond.value, cond_type, cond.op);
        }

        bool Evaluator::MatchComposite(const std::string& bson_doc,
                                       const Condition&   cond) const {
            switch (cond.logic) {
                case LogicOp::AND:
                    for (const auto& sub : cond.sub_conditions)
                        if (!Match(bson_doc, sub)) return false;
                    return true;

                case LogicOp::OR:
                    for (const auto& sub : cond.sub_conditions)
                        if (Match(bson_doc, sub)) return true;
                    return false;

                case LogicOp::NOR:
                    for (const auto& sub : cond.sub_conditions)
                        if (Match(bson_doc, sub)) return false;
                    return true;

                case LogicOp::NOT:
                    if (cond.sub_conditions.empty()) return true;
                    return !Match(bson_doc, cond.sub_conditions[0]);
            }
            return false;
        }

// ──────────────────────────────────────────────────────────────
// Apply
// ──────────────────────────────────────────────────────────────

        std::string Evaluator::Apply(const std::string& bson_doc,
                                     const UpdateSpec&  spec) const {
            std::string result = bson_doc;
            for (const auto& op : spec.operations) {
                result = ApplyOperation(result, op);
            }
            return result;
        }

        std::string Evaluator::ApplyOperation(const std::string&     doc,
                                              const UpdateOperation& op) const {
            switch (op.op) {

                case UpdateOp::Set:
                    return adapter_->SetField(doc, op.field, op.value, op.value_type);

                case UpdateOp::Unset:
                    return adapter_->UnsetField(doc, op.field);

                case UpdateOp::Rename:
                    return adapter_->RenameField(doc, op.field, op.value);

                case UpdateOp::Inc: {
                    FieldValue fv = adapter_->GetField(doc, op.field);
                    double current = 0.0;
                    if (fv.found && fv.IsNumber()) {
                        try { current = std::stod(fv.raw); } catch (...) {}
                    }
                    double delta = 0.0;
                    try { delta = std::stod(op.value); } catch (...) {}
                    double result = current + delta;

                    UpdateValueType uvt = op.value_type;
                    std::string new_val;
                    if (uvt == UpdateValueType::Int64) {
                        new_val = std::to_string(static_cast<int64_t>(result));
                    } else {
                        new_val = std::to_string(result);
                        // حذف صفرهای اضافه
                        auto dot_pos = new_val.find('.');
                        if (dot_pos != std::string::npos) {
                            new_val.erase(new_val.find_last_not_of('0') + 1);
                            if (new_val.back() == '.') new_val.pop_back();
                        }
                    }
                    return adapter_->SetField(doc, op.field, new_val, uvt);
                }

                case UpdateOp::Mul: {
                    FieldValue fv = adapter_->GetField(doc, op.field);
                    double current = 1.0;
                    if (fv.found && fv.IsNumber()) {
                        try { current = std::stod(fv.raw); } catch (...) {}
                    }
                    double factor = 1.0;
                    try { factor = std::stod(op.value); } catch (...) {}
                    double result = current * factor;
                    UpdateValueType uvt = op.value_type;
                    std::string new_val = (uvt == UpdateValueType::Int64)
                                          ? std::to_string(static_cast<int64_t>(result))
                                          : std::to_string(result);
                    return adapter_->SetField(doc, op.field, new_val, uvt);
                }

                case UpdateOp::Min: {
                    FieldValue fv = adapter_->GetField(doc, op.field);
                    if (!fv.found) return adapter_->SetField(doc, op.field, op.value, op.value_type);
                    // اگر مقدار جدید کمتر است، جایگزین کن
                    bool should_update = detail::CompareScalar(
                            op.value, op.value_type == UpdateValueType::Int64 ? ValueType::Int64 : ValueType::Float64,
                            fv.raw,   fv.type,
                            Op::LT);
                    return should_update
                           ? adapter_->SetField(doc, op.field, op.value, op.value_type)
                           : doc;
                }

                case UpdateOp::Max: {
                    FieldValue fv = adapter_->GetField(doc, op.field);
                    if (!fv.found) return adapter_->SetField(doc, op.field, op.value, op.value_type);
                    bool should_update = detail::CompareScalar(
                            op.value, op.value_type == UpdateValueType::Int64 ? ValueType::Int64 : ValueType::Float64,
                            fv.raw,   fv.type,
                            Op::GT);
                    return should_update
                           ? adapter_->SetField(doc, op.field, op.value, op.value_type)
                           : doc;
                }

                case UpdateOp::CurrentDate: {
                    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                    return adapter_->SetField(doc, op.field,
                                              std::to_string(now_ms),
                                              UpdateValueType::Int64);
                }

                case UpdateOp::Push:
                    return adapter_->PushToArray(doc, op.field, op.value, op.value_type);

                case UpdateOp::PushAll: {
                    std::string result = doc;
                    for (const auto& elem : op.values)
                        result = adapter_->PushToArray(result, op.field, elem, op.value_type);
                    return result;
                }

                case UpdateOp::Pull:
                    return adapter_->PullFromArray(doc, op.field, op.value);

                case UpdateOp::PullAll:
                    return adapter_->PullAllFromArray(doc, op.field, op.values);

                case UpdateOp::AddToSet: {
                    FieldValue fv = adapter_->GetField(doc, op.field);
                    if (fv.found && fv.raw.find("\"" + op.value + "\"") != std::string::npos)
                        return doc; // قبلاً وجود دارد
                    return adapter_->PushToArray(doc, op.field, op.value, op.value_type);
                }

                case UpdateOp::Pop:
                    return adapter_->PopArray(doc, op.field, op.value != "-1");
            }
            return doc;
        }

// ──────────────────────────────────────────────────────────────
// Projection و ExtractField
// ──────────────────────────────────────────────────────────────

        std::string Evaluator::ApplyProjection(const std::string&  bson_doc,
                                               const QueryOptions& opts) const {
            if (opts.projection.empty()) return bson_doc;
            return adapter_->Project(bson_doc, opts.projection, opts.projection_exclude);
        }

        FieldValue Evaluator::ExtractField(const std::string& bson_doc,
                                           const std::string& field_path) const {
            return adapter_->GetField(bson_doc, field_path);
        }

    } // namespace query
} // namespace nexora
