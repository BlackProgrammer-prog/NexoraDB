#include "CompiledUpdate.h"
#include "NumericCompare.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <system_error>
#include <stdexcept>

namespace nexora::query {
namespace {

using Json = nlohmann::ordered_json;

template <typename Number>
bool ParseNumber(std::string_view text, Number& output) noexcept {
    const auto result = std::from_chars(
            text.data(), text.data() + text.size(), output);
    return result.ec == std::errc{} &&
           result.ptr == text.data() + text.size();
}

bool ParseValue(std::string_view raw, UpdateValueType type,
                Json& value, std::string& error) {
    switch (type) {
        case UpdateValueType::String:
            value = raw;
            return true;
        case UpdateValueType::Int64: {
            std::int64_t parsed = 0;
            if (!ParseNumber(raw, parsed)) {
                error = "invalid Int64 update value";
                return false;
            }
            value = parsed;
            return true;
        }
        case UpdateValueType::Float64: {
            double parsed = 0.0;
            if (!ParseNumber(raw, parsed) || !std::isfinite(parsed)) {
                error = "invalid Float64 update value";
                return false;
            }
            value = parsed;
            return true;
        }
        case UpdateValueType::Bool:
            if (raw == "true" || raw == "1") value = true;
            else if (raw == "false" || raw == "0") value = false;
            else {
                error = "invalid Bool update value";
                return false;
            }
            return true;
        case UpdateValueType::Null:
            value = nullptr;
            return true;
        case UpdateValueType::Array:
            value = Json::parse(raw, nullptr, false);
            if (!value.is_array()) {
                error = "invalid Array update value";
                return false;
            }
            return true;
        case UpdateValueType::Object:
            value = Json::parse(raw, nullptr, false);
            if (!value.is_object()) {
                error = "invalid Object update value";
                return false;
            }
            return true;
    }
    error = "unknown update value type";
    return false;
}

bool CheckedAdd(std::int64_t left, std::int64_t right,
                std::int64_t& result) noexcept {
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right))
        return false;
    result = left + right;
    return true;
}

bool CheckedMultiply(std::int64_t left, std::int64_t right,
                     std::int64_t& result) noexcept {
    if (left == 0 || right == 0) {
        result = 0;
        return true;
    }
    if ((left == -1 && right == std::numeric_limits<std::int64_t>::min()) ||
        (right == -1 && left == std::numeric_limits<std::int64_t>::min()))
        return false;
    if (left > 0) {
        if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() / right) ||
            (right < 0 && right < std::numeric_limits<std::int64_t>::min() / left))
            return false;
    } else {
        if ((right > 0 && left < std::numeric_limits<std::int64_t>::min() / right) ||
            (right < 0 && left < std::numeric_limits<std::int64_t>::max() / right))
            return false;
    }
    result = left * right;
    return true;
}

Json* ResolveParent(Json& root, const std::vector<std::string>& components,
                    bool create) {
    if (components.empty()) return nullptr;
    Json* current = &root;
    for (std::size_t i = 0; i + 1 < components.size(); ++i) {
        if (!current->is_object()) {
            if (!create) return nullptr;
            throw std::invalid_argument("Update path traverses a non-object");
        }
        auto found = current->find(components[i]);
        if (found == current->end()) {
            if (!create) return nullptr;
            found = current->emplace(components[i], Json::object()).first;
        }
        if (!found->is_object()) {
            if (!create) return nullptr;
            throw std::invalid_argument("Update path traverses a non-object");
        }
        current = &*found;
    }
    return current;
}

Json* FindValue(Json& root, const std::vector<std::string>& components) {
    Json* parent = ResolveParent(root, components, false);
    if (!parent || !parent->is_object()) return nullptr;
    auto found = parent->find(components.back());
    return found == parent->end() ? nullptr : &*found;
}

void SetValue(Json& root, const std::vector<std::string>& components,
              Json value) {
    Json* parent = ResolveParent(root, components, true);
    if (parent) (*parent)[components.back()] = std::move(value);
}

void EraseValue(Json& root, const std::vector<std::string>& components) {
    Json* parent = ResolveParent(root, components, false);
    if (parent && parent->is_object()) parent->erase(components.back());
}

} // namespace

struct CompiledUpdate::Operation {
    UpdateOp op = UpdateOp::Set;
    CompiledFieldPath path{"invalid"};
    CompiledFieldPath rename_path{"invalid"};
    Json value;
    std::vector<Json> values;
};

CompiledUpdate::~CompiledUpdate() = default;

CompiledUpdate::CompiledUpdate(const UpdateSpec& spec) {
    operations_.reserve(spec.operations.size());
    for (const auto& source : spec.operations) {
        Operation operation;
        operation.op = source.op;
        operation.path = CompiledFieldPath(source.field);
        if (!operation.path.valid()) {
            error_ = operation.path.error();
            return;
        }
        if (operation.path.components().front() == "_id") {
            error_ = "Document _id is immutable";
            return;
        }

        if (source.op == UpdateOp::Rename) {
            operation.rename_path = CompiledFieldPath(source.value);
            if (!operation.rename_path.valid()) {
                error_ = operation.rename_path.error();
                return;
            }
            if (operation.rename_path.components().front() == "_id" ||
                source.value == source.field || source.value.starts_with(source.field + ".") ||
                source.field.starts_with(source.value + ".")) {
                error_ = "Invalid overlapping or immutable rename path";
                return;
            }
        } else if (source.op != UpdateOp::Unset &&
                   source.op != UpdateOp::CurrentDate &&
                   source.op != UpdateOp::PushAll &&
                   source.op != UpdateOp::PullAll) {
            if (!ParseValue(source.value, source.value_type,
                            operation.value, error_)) return;
        }
        if ((source.op == UpdateOp::Inc || source.op == UpdateOp::Mul ||
             source.op == UpdateOp::Min || source.op == UpdateOp::Max) &&
            !operation.value.is_number()) {
            error_ = "Numeric update requires a numeric operand";
            return;
        }

        if (source.op == UpdateOp::PushAll ||
            source.op == UpdateOp::PullAll) {
            operation.values.reserve(source.values.size());
            for (const auto& raw : source.values) {
                Json value;
                if (!ParseValue(raw, source.value_type,
                                value, error_)) return;
                operation.values.push_back(std::move(value));
            }
        }
        operations_.push_back(std::move(operation));
    }
}

CompiledUpdate::Result CompiledUpdate::ApplyJson(
        std::string_view json_document) const {
    if (!valid()) return {false, {}, error_};
    try {
    Json document = Json::parse(json_document, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return {false, {}, "update input is not a valid JSON object"};
    }

    for (const auto& operation : operations_) {
        const auto& path = operation.path.components();
        switch (operation.op) {
            case UpdateOp::Set:
                SetValue(document, path, operation.value);
                break;
            case UpdateOp::Unset:
                EraseValue(document, path);
                break;
            case UpdateOp::Rename: {
                Json* current = FindValue(document, path);
                if (current) {
                    Json moved = std::move(*current);
                    EraseValue(document, path);
                    SetValue(document,
                             operation.rename_path.components(),
                             std::move(moved));
                }
                break;
            }
            case UpdateOp::Inc:
            case UpdateOp::Mul: {
                Json* current = FindValue(document, path);
                if (current && !current->is_number())
                    return {false, {}, "Numeric update target is not a number"};
                if (current && current->is_number_unsigned() &&
                    current->get<std::uint64_t>() > static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max()))
                    return {false, {}, "Numeric update target exceeds Int64"};
                const bool integer_result = operation.value.is_number_integer() &&
                        (!current || current->is_number_integer());
                if (integer_result) {
                    const std::int64_t base = current
                            ? current->get<std::int64_t>()
                            : (operation.op == UpdateOp::Mul ? 1 : 0);
                    const std::int64_t operand =
                            operation.value.get<std::int64_t>();
                    std::int64_t result = 0;
                    const bool valid = operation.op == UpdateOp::Inc
                            ? CheckedAdd(base, operand, result)
                            : CheckedMultiply(base, operand, result);
                    if (!valid) {
                        return {false, {}, "integer update overflow"};
                    }
                    SetValue(document, path, result);
                    break;
                }
                const double base = current && current->is_number()
                        ? current->get<double>()
                        : (operation.op == UpdateOp::Mul ? 1.0 : 0.0);
                const double operand = operation.value.get<double>();
                const double result = operation.op == UpdateOp::Inc
                        ? base + operand : base * operand;
                if (!std::isfinite(result)) {
                    return {false, {}, "floating-point update overflow"};
                }
                SetValue(document, path, result);
                break;
            }
            case UpdateOp::Min:
            case UpdateOp::Max: {
                Json* current = FindValue(document, path);
                if (current && !current->is_number())
                    return {false, {}, "MIN/MAX target is not a number"};
                if (!current || !current->is_number() ||
                    !operation.value.is_number()) {
                    if (!current) SetValue(document, path, operation.value);
                    break;
                }
                if (current->is_number_unsigned() && current->get<std::uint64_t>() >
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                    return {false, {}, "MIN/MAX target exceeds Int64"};
                int order = 0; // candidate relative to existing
                if (current->is_number_integer() && operation.value.is_number_integer()) {
                    const auto existing = current->get<std::int64_t>();
                    const auto candidate = operation.value.get<std::int64_t>();
                    order = candidate < existing ? -1 : (candidate > existing ? 1 : 0);
                } else if (current->is_number_integer()) {
                    order = -CompareIntDouble(current->get<std::int64_t>(), operation.value.get<double>());
                } else if (operation.value.is_number_integer()) {
                    order = CompareIntDouble(operation.value.get<std::int64_t>(), current->get<double>());
                } else {
                    const auto existing = current->get<double>();
                    const auto candidate = operation.value.get<double>();
                    order = candidate < existing ? -1 : (candidate > existing ? 1 : 0);
                }
                if ((operation.op == UpdateOp::Min && order < 0) ||
                    (operation.op == UpdateOp::Max && order > 0)) {
                    SetValue(document, path, operation.value);
                }
                break;
            }
            case UpdateOp::CurrentDate: {
                const auto milliseconds =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now()
                                        .time_since_epoch()).count();
                SetValue(document, path, milliseconds);
                break;
            }
            case UpdateOp::Push:
            case UpdateOp::AddToSet: {
                Json* array = FindValue(document, path);
                if (array && !array->is_array())
                    return {false, {}, "Array update target is not an array"};
                if (!array) {
                    SetValue(document, path, Json::array());
                    array = FindValue(document, path);
                }
                if (operation.op == UpdateOp::Push ||
                    std::find(array->begin(), array->end(), operation.value) ==
                            array->end()) {
                    array->push_back(operation.value);
                }
                break;
            }
            case UpdateOp::PushAll: {
                Json* array = FindValue(document, path);
                if (array && !array->is_array())
                    return {false, {}, "Array update target is not an array"};
                if (!array) {
                    SetValue(document, path, Json::array());
                    array = FindValue(document, path);
                }
                for (const auto& item : operation.values)
                    array->push_back(item);
                break;
            }
            case UpdateOp::Pull:
            case UpdateOp::PullAll: {
                Json* array = FindValue(document, path);
                if (array && !array->is_array())
                    return {false, {}, "Array update target is not an array"};
                if (!array) break;
                array->erase(std::remove_if(
                        array->begin(), array->end(),
                        [&](const Json& item) {
                            if (operation.op == UpdateOp::Pull)
                                return item == operation.value;
                            return std::find(operation.values.begin(),
                                             operation.values.end(), item) !=
                                   operation.values.end();
                        }), array->end());
                break;
            }
            case UpdateOp::Pop: {
                Json* array = FindValue(document, path);
                if (array && !array->is_array())
                    return {false, {}, "Array update target is not an array"};
                if (!array || array->empty()) break;
                const bool from_end = operation.value.is_number_integer()
                        ? operation.value.get<std::int64_t>() != -1 : true;
                if (from_end) array->erase(array->end() - 1);
                else array->erase(array->begin());
                break;
            }
        }
    }

    return {true, document.dump(), {}};
    } catch (const std::exception& exception) {
        return {false, {}, std::string("Invalid update: ") + exception.what()};
    }
}

} // namespace nexora::query
