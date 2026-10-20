#include "DocumentView.h"

#include <limits>

namespace nexora::query {

CompiledFieldPath::CompiledFieldPath(std::string_view path) {
    if (path.empty() || path.find('\0') != std::string_view::npos) {
        error_ = "field path is empty or contains NUL";
        return;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t separator = path.find('.', start);
        const std::size_t end = separator == std::string_view::npos
                ? path.size() : separator;
        if (end == start) {
            error_ = "field path contains an empty component";
            components_.clear();
            return;
        }
        if (end - start > 1024) {
            error_ = "field path component exceeds 1 KiB";
            components_.clear();
            return;
        }
        components_.emplace_back(path.substr(start, end - start));
        if (components_.size() > 100) {
            error_ = "field path exceeds 100 components";
            components_.clear();
            return;
        }
        if (separator == std::string_view::npos) break;
        start = separator + 1;
    }
}

DocumentView::DocumentView(std::string_view bson_payload) noexcept
        : bytes_(bson_payload) {
    if (bytes_.size() < 5 ||
        bytes_.size() > std::numeric_limits<std::uint32_t>::max()) {
        return;
    }
    valid_ = bson_init_static(
            &document_,
            reinterpret_cast<const std::uint8_t*>(bytes_.data()),
            static_cast<std::uint32_t>(bytes_.size()));
}

DocumentValueView DocumentView::Get(
        const CompiledFieldPath& path) const noexcept {
    if (!valid_ || !path.valid()) return {};

    bson_t current = document_;
    bson_iter_t iterator;
    const auto& components = path.components();
    for (std::size_t index = 0; index < components.size(); ++index) {
        if (!bson_iter_init_find(&iterator, &current,
                                 components[index].c_str())) {
            return {};
        }
        if (index + 1 < components.size()) {
            const std::uint8_t* child_bytes = nullptr;
            std::uint32_t child_size = 0;
            if (BSON_ITER_HOLDS_DOCUMENT(&iterator)) {
                bson_iter_document(&iterator, &child_size, &child_bytes);
            } else if (BSON_ITER_HOLDS_ARRAY(&iterator)) {
                bson_iter_array(&iterator, &child_size, &child_bytes);
            } else {
                return {};
            }
            if (!bson_init_static(&current, child_bytes, child_size)) return {};
        }
    }

    if (BSON_ITER_HOLDS_NULL(&iterator) ||
        BSON_ITER_HOLDS_UNDEFINED(&iterator)) {
        return {DocumentValueType::Null};
    }
    if (BSON_ITER_HOLDS_UTF8(&iterator)) {
        std::uint32_t length = 0;
        const char* value = bson_iter_utf8(&iterator, &length);
        DocumentValueView result{DocumentValueType::String};
        result.string_value = std::string_view(value, length);
        return result;
    }
    if (BSON_ITER_HOLDS_INT32(&iterator)) {
        DocumentValueView result{DocumentValueType::Int64};
        result.int_value = bson_iter_int32(&iterator);
        return result;
    }
    if (BSON_ITER_HOLDS_INT64(&iterator) ||
        BSON_ITER_HOLDS_DATE_TIME(&iterator)) {
        DocumentValueView result{DocumentValueType::Int64};
        result.int_value = bson_iter_as_int64(&iterator);
        return result;
    }
    if (BSON_ITER_HOLDS_TIMESTAMP(&iterator)) {
        std::uint32_t timestamp = 0;
        std::uint32_t increment = 0;
        bson_iter_timestamp(&iterator, &timestamp, &increment);
        DocumentValueView result{DocumentValueType::Int64};
        result.int_value = timestamp;
        return result;
    }
    if (BSON_ITER_HOLDS_DOUBLE(&iterator)) {
        DocumentValueView result{DocumentValueType::Float64};
        result.double_value = bson_iter_double(&iterator);
        return result;
    }
    if (BSON_ITER_HOLDS_BOOL(&iterator)) {
        DocumentValueView result{DocumentValueType::Bool};
        result.bool_value = bson_iter_bool(&iterator);
        return result;
    }
    if (BSON_ITER_HOLDS_DOCUMENT(&iterator)) {
        return {DocumentValueType::Document};
    }
    if (BSON_ITER_HOLDS_ARRAY(&iterator)) {
        return {DocumentValueType::Array};
    }
    if (BSON_ITER_HOLDS_BINARY(&iterator)) {
        return {DocumentValueType::Binary};
    }
    return {DocumentValueType::Other};
}

} // namespace nexora::query
