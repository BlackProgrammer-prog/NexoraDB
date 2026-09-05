#include "IndexCodec.h"

#include <bit>
#include <cstring>

namespace nexora::core::indexv2 {
namespace {
void AppendU32(std::string& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<char>((value >> shift) & 0xff));
}

void AppendU64(std::string& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<char>((value >> shift) & 0xff));
}

void AppendLengthPrefixed(std::string& out, const std::string& value) {
    AppendU32(out, static_cast<std::uint32_t>(value.size()));
    out.append(value);
}

void AppendEscapedString(std::string& out, const std::string& value) {
    for (const unsigned char byte : value) {
        if (byte == 0) {
            out.push_back('\0');
            out.push_back(static_cast<char>(0xff));
        } else {
            out.push_back(static_cast<char>(byte));
        }
    }
    out.push_back('\0');
    out.push_back('\0');
}

bool AppendValue(std::string& out, const query::FieldValue& value) {
    if (!value.found || value.raw == "null") return false;
    query::ValueType type = value.type;
    if (value.raw.empty()) type = query::ValueType::String;
    out.push_back(static_cast<char>(type));
    try {
        switch (type) {
            case query::ValueType::Int64: {
                const auto signed_value = std::stoll(value.raw);
                AppendU64(out, static_cast<std::uint64_t>(signed_value) ^
                               (std::uint64_t{1} << 63));
                break;
            }
            case query::ValueType::Float64: {
                std::uint64_t bits = std::bit_cast<std::uint64_t>(
                        std::stod(value.raw));
                bits = (bits & (std::uint64_t{1} << 63))
                        ? ~bits : bits ^ (std::uint64_t{1} << 63);
                AppendU64(out, bits);
                break;
            }
            case query::ValueType::Bool:
                out.push_back(value.raw == "true" || value.raw == "1" ? 1 : 0);
                break;
            case query::ValueType::String:
                AppendEscapedString(out, value.raw);
                break;
            case query::ValueType::Null:
                return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}
} // namespace

std::optional<std::string> EncodeTuple(
        const std::string& document,
        const std::vector<std::string>& fields) {
    query::Evaluator evaluator;
    std::vector<query::FieldValue> values;
    values.reserve(fields.size());
    for (const auto& field : fields) {
        values.push_back(evaluator.ExtractField(document, field));
    }
    return EncodeValues(values);
}

std::optional<std::string> EncodeValues(
        const std::vector<query::FieldValue>& values) {
    std::string tuple;
    for (const auto& value : values)
        if (!AppendValue(tuple, value)) return std::nullopt;
    return tuple;
}

std::string IndexPrefix(const std::string& collection,
                        const std::string& index_id) {
    std::string key = CollectionPrefix(collection);
    AppendLengthPrefixed(key, index_id);
    return key;
}

std::string CollectionPrefix(const std::string& collection) {
    std::string key = "idx2";
    AppendLengthPrefixed(key, collection);
    return key;
}

std::string EntryKey(const std::string& collection,
                     const std::string& index_id,
                     const std::string& tuple,
                     const std::string& document_id) {
    std::string key = IndexPrefix(collection, index_id);
    key.push_back('E');
    key.append(tuple);
    AppendEscapedString(key, document_id);
    return key;
}

std::string UniqueKey(const std::string& collection,
                      const std::string& index_id,
                      const std::string& tuple) {
    std::string key = IndexPrefix(collection, index_id);
    key.push_back('U');
    key.append(tuple);
    return key;
}
} // namespace nexora::core::indexv2
