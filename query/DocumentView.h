#ifndef NEXORADB_DOCUMENT_VIEW_H
#define NEXORADB_DOCUMENT_VIEW_H

#pragma once

#include <bson/bson.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nexora::query {

enum class DocumentValueType : std::uint8_t {
    Missing,
    Null,
    String,
    Int64,
    Float64,
    Bool,
    Document,
    Array,
    Binary,
    Other
};

struct DocumentValueView {
    DocumentValueView() noexcept = default;
    DocumentValueView(DocumentValueType value_type) noexcept
            : type(value_type) {}

    DocumentValueType type = DocumentValueType::Missing;
    std::string_view string_value;
    std::int64_t int_value = 0;
    double double_value = 0.0;
    bool bool_value = false;

    [[nodiscard]] bool found() const noexcept {
        return type != DocumentValueType::Missing;
    }
    [[nodiscard]] bool is_number() const noexcept {
        return type == DocumentValueType::Int64 ||
               type == DocumentValueType::Float64;
    }
};

class CompiledFieldPath final {
public:
    explicit CompiledFieldPath(std::string_view path);

    [[nodiscard]] bool valid() const noexcept { return error_.empty(); }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] const std::vector<std::string>& components() const noexcept {
        return components_;
    }

private:
    std::vector<std::string> components_;
    std::string error_;
};

/** Immutable zero-copy view over one validated BSON document. */
class DocumentView final {
public:
    explicit DocumentView(std::string_view bson_payload) noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] DocumentValueView Get(
            const CompiledFieldPath& path) const noexcept;

private:
    std::string_view bytes_;
    bson_t document_{};
    bool valid_ = false;
};

} // namespace nexora::query

#endif // NEXORADB_DOCUMENT_VIEW_H
