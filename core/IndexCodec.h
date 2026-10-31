#pragma once

#include "query/Evaluator.h"
#include "query/DocumentView.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nexora::core::indexv2 {

constexpr std::uint32_t kFormatVersion = 3;

std::optional<std::string> EncodeTuple(
        const std::string& document,
        const std::vector<std::string>& fields);
std::optional<std::string> EncodeTuple(
        const query::DocumentView& document,
        const std::vector<query::CompiledFieldPath>& fields);
std::string EntryKeyFromPrefix(const std::string& prefix,
                              const std::string& encoded_tuple,
                              const std::string& document_id);

std::optional<std::string> EncodeValues(
        const std::vector<query::FieldValue>& values);

std::string IndexPrefix(
        const std::string& collection,
        const std::string& index_id);

std::string CollectionPrefix(const std::string& collection);

std::string EntryKey(
        const std::string& collection,
        const std::string& index_id,
        const std::string& encoded_tuple,
        const std::string& document_id);

std::string UniqueKey(
        const std::string& collection,
        const std::string& index_id,
        const std::string& encoded_tuple);

} // namespace nexora::core::indexv2
