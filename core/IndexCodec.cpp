#include "IndexCodec.h"
#include "DocumentCodec.h"
#include "query/DocumentView.h"

#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <system_error>

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

void AppendEscapedString(std::string& out, std::string_view value) {
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
    switch (type) {
        case query::ValueType::Int64: {
                std::int64_t signed_value = 0;
                const auto parsed = std::from_chars(
                        value.raw.data(), value.raw.data() + value.raw.size(),
                        signed_value);
                if (parsed.ec != std::errc{} ||
                    parsed.ptr != value.raw.data() + value.raw.size()) return false;
                AppendU64(out, static_cast<std::uint64_t>(signed_value) ^
                               (std::uint64_t{1} << 63));
                break;
            }
        case query::ValueType::Float64: {
                double floating = 0.0;
                const auto parsed = std::from_chars(
                        value.raw.data(), value.raw.data() + value.raw.size(),
                        floating);
                if (parsed.ec != std::errc{} ||
                    parsed.ptr != value.raw.data() + value.raw.size() ||
                    !std::isfinite(floating)) return false;
                std::uint64_t bits = std::bit_cast<std::uint64_t>(floating);
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
    return true;
}

bool AppendDocumentValue(std::string& out,
                         const query::DocumentValueView& value) {
    switch (value.type) {
        case query::DocumentValueType::String:
            out.push_back(static_cast<char>(query::ValueType::String));
            AppendEscapedString(out, value.string_value);
            return true;
        case query::DocumentValueType::Int64:
            out.push_back(static_cast<char>(query::ValueType::Int64));
            AppendU64(out, static_cast<std::uint64_t>(value.int_value) ^
                           (std::uint64_t{1} << 63));
            return true;
        case query::DocumentValueType::Float64: {
            if (!std::isfinite(value.double_value)) return false;
            out.push_back(static_cast<char>(query::ValueType::Float64));
            std::uint64_t bits = std::bit_cast<std::uint64_t>(
                    value.double_value);
            bits = (bits & (std::uint64_t{1} << 63))
                    ? ~bits : bits ^ (std::uint64_t{1} << 63);
            AppendU64(out, bits);
            return true;
        }
        case query::DocumentValueType::Bool:
            out.push_back(static_cast<char>(query::ValueType::Bool));
            out.push_back(value.bool_value ? 1 : 0);
            return true;
        default:
            return false;
    }
}
} // namespace

std::optional<std::string> EncodeTuple(
        const std::string& document,
        const std::vector<std::string>& fields) {
    std::string owned_bson;
    std::string_view bson_payload;
    switch (DocumentCodec::Detect(document)) {
        case DocumentCodec::Format::LegacyJson: {
            auto encoded = DocumentCodec::EncodeJson(document);
            if (!encoded.success) return std::nullopt;
            owned_bson = std::move(encoded.value);
            bson_payload = std::string_view(owned_bson).substr(
                    DocumentCodec::kEnvelopeSize);
            break;
        }
        case DocumentCodec::Format::BsonV1:
            bson_payload = std::string_view(document).substr(
                    DocumentCodec::kEnvelopeSize);
            break;
        case DocumentCodec::Format::UnknownEnvelope:
            return std::nullopt;
    }
    const query::DocumentView view(bson_payload);
    if (!view.valid()) return std::nullopt;
    std::string tuple;
    for (const auto& field : fields) {
        const query::CompiledFieldPath path(field);
        if (!path.valid() || !AppendDocumentValue(tuple, view.Get(path)))
            return std::nullopt;
    }
    return tuple;
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
