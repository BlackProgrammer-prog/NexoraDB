#include "Evaluator.h"

#include "CompiledQuery.h"
#include "CompiledUpdate.h"
#include "DocumentView.h"

#include <bson/bson.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <string_view>
#include <stdexcept>

namespace nexora::query {
namespace {

using Json = nlohmann::ordered_json;

const Json* FindJsonValue(const Json& document, std::string_view path) {
    if (path.empty()) return nullptr;
    const Json* current = &document;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t separator = path.find('.', start);
        const std::string component(path.substr(
                start, separator == std::string_view::npos
                        ? path.size() - start : separator - start));
        if (component.empty() || !current->is_object()) return nullptr;
        const auto found = current->find(component);
        if (found == current->end()) return nullptr;
        current = &*found;
        if (separator == std::string_view::npos) return current;
        start = separator + 1;
    }
    return nullptr;
}

void SetJsonValue(Json& document, std::string_view path, const Json& value) {
    Json* current = &document;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t separator = path.find('.', start);
        const std::string component(path.substr(
                start, separator == std::string_view::npos
                        ? path.size() - start : separator - start));
        if (component.empty()) return;
        if (separator == std::string_view::npos) {
            (*current)[component] = value;
            return;
        }
        Json& child = (*current)[component];
        if (!child.is_object()) child = Json::object();
        current = &child;
        start = separator + 1;
    }
}

FieldValue ToFieldValue(const Json& value) {
    if (value.is_null()) return {"null", ValueType::Null, true};
    if (value.is_string())
        return {value.get<std::string>(), ValueType::String, true};
    if (value.is_boolean())
        return {value.get<bool>() ? "true" : "false", ValueType::Bool, true};
    if (value.is_number_integer() || value.is_number_unsigned())
        return {value.dump(), ValueType::Int64, true};
    if (value.is_number_float())
        return {value.dump(), ValueType::Float64, true};
    return {value.dump(), ValueType::String, true};
}

std::string ApplyOne(const std::string& document, UpdateOperation operation) {
    UpdateSpec spec;
    spec.Add(std::move(operation));
    const CompiledUpdate compiled(spec);
    if (!compiled.valid()) throw std::invalid_argument(compiled.error());
    auto result = compiled.ApplyJson(document);
    if (!result.success) throw std::invalid_argument(result.error);
    return std::move(result.document);
}

// libbson annotates bson_t with an alignment attribute that GCC warns about
// when used as a smart-pointer template argument. Ownership stays local and
// is released immediately after evaluation.
bson_t* JsonToBson(const std::string& json) {
    bson_error_t error{};
    return bson_new_from_json(
                    reinterpret_cast<const std::uint8_t*>(json.data()),
                    static_cast<ssize_t>(json.size()), &error);
}

} // namespace

FieldValue JsonAdapter::GetField(const std::string& doc,
                                 const std::string& field_path) const {
    const Json document = Json::parse(doc, nullptr, false);
    if (document.is_discarded()) return {};
    const Json* value = FindJsonValue(document, field_path);
    return value ? ToFieldValue(*value) : FieldValue{};
}

std::string JsonAdapter::SetField(const std::string& doc,
                                  const std::string& field_path,
                                  const std::string& value,
                                  UpdateValueType type) const {
    return ApplyOne(doc, {UpdateOp::Set, field_path, value, {}, type});
}

std::string JsonAdapter::UnsetField(const std::string& doc,
                                    const std::string& field_path) const {
    return ApplyOne(doc, UpdateOperation::MakeUnset(field_path));
}

std::string JsonAdapter::PushToArray(const std::string& doc,
                                     const std::string& field_path,
                                     const std::string& element,
                                     UpdateValueType type) const {
    return ApplyOne(doc, {UpdateOp::Push, field_path, element, {}, type});
}

std::string JsonAdapter::PullFromArray(const std::string& doc,
                                       const std::string& field_path,
                                       const std::string& value) const {
    return ApplyOne(doc, UpdateOperation::MakePull(field_path, value));
}

std::string JsonAdapter::PullAllFromArray(
        const std::string& doc, const std::string& field_path,
        const std::vector<std::string>& values) const {
    return ApplyOne(doc, {UpdateOp::PullAll, field_path, "", values,
                          UpdateValueType::String});
}

std::string JsonAdapter::PopArray(const std::string& doc,
                                  const std::string& field_path,
                                  bool from_end) const {
    return ApplyOne(doc, {UpdateOp::Pop, field_path,
                          from_end ? "1" : "-1", {},
                          UpdateValueType::Int64});
}

std::string JsonAdapter::Project(const std::string& doc,
                                 const std::vector<std::string>& fields,
                                 bool exclude) const {
    if (fields.empty()) return doc;
    Json document = Json::parse(doc, nullptr, false);
    if (document.is_discarded() || !document.is_object()) return doc;
    if (exclude) {
        UpdateSpec spec;
        for (const auto& field : fields) spec.Unset(field);
        const CompiledUpdate compiled(spec);
        auto result = compiled.ApplyJson(doc);
        return result.success ? std::move(result.document) : doc;
    }

    Json projected = Json::object();
    if (const Json* id = FindJsonValue(document, "_id"))
        projected["_id"] = *id;
    for (const auto& field : fields) {
        if (field == "_id") continue;
        if (const Json* value = FindJsonValue(document, field))
            SetJsonValue(projected, field, *value);
    }
    return projected.dump();
}

std::string JsonAdapter::RenameField(const std::string& doc,
                                     const std::string& old_field,
                                     const std::string& new_field) const {
    return ApplyOne(doc, {UpdateOp::Rename, old_field, new_field, {},
                          UpdateValueType::String});
}

Evaluator::Evaluator() : adapter_(std::make_unique<JsonAdapter>()) {}

Evaluator::Evaluator(std::unique_ptr<IBsonAdapter> adapter)
        : adapter_(std::move(adapter)) {}

bool Evaluator::Match(const std::string& document,
                      const Condition& condition) const {
    const CompiledQuery compiled(condition);
    if (!compiled.valid()) throw std::invalid_argument(compiled.error());

    if (document.size() >= 8 &&
        std::string_view(document).substr(0, 4) == "NXD1") {
        const DocumentView view(std::string_view(document).substr(8));
        return view.valid() && compiled.Match(view);
    }
    bson_t* bson = JsonToBson(document);
    if (!bson) return false;
    const DocumentView view(std::string_view(
            reinterpret_cast<const char*>(bson_get_data(bson)),
            bson->len));
    bool matched;
    try {
        matched = view.valid() && compiled.Match(view);
    } catch (...) {
        bson_destroy(bson);
        throw;
    }
    bson_destroy(bson);
    return matched;
}

std::string Evaluator::Apply(const std::string& document,
                             const UpdateSpec& update_spec) const {
    const CompiledUpdate compiled(update_spec);
    if (!compiled.valid()) throw std::invalid_argument(compiled.error());
    auto result = compiled.ApplyJson(document);
    if (!result.success) throw std::invalid_argument(result.error);
    return std::move(result.document);
}

std::string Evaluator::ApplyProjection(const std::string& document,
                                       const QueryOptions& options) const {
    return adapter_->Project(document, options.projection,
                             options.projection_exclude);
}

FieldValue Evaluator::ExtractField(const std::string& document,
                                   const std::string& field_path) const {
    return adapter_->GetField(document, field_path);
}

} // namespace nexora::query
