#include "DocumentCodec.h"

#include <bson/bson.h>

#include <array>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace nexora::core {
namespace {

constexpr std::array<char, DocumentCodec::kEnvelopeSize> kBsonEnvelope{
        'N', 'X', 'D', '1', 1, 0, 0, 0};

class OwnedBson final {
public:
    explicit OwnedBson(bson_t* value) noexcept : value_(value) {}
    ~OwnedBson() { if (value_) bson_destroy(value_); }
    OwnedBson(const OwnedBson&) = delete;
    OwnedBson& operator=(const OwnedBson&) = delete;
    [[nodiscard]] bson_t* get() const noexcept { return value_; }
    [[nodiscard]] bson_t* operator->() const noexcept { return value_; }
    [[nodiscard]] bson_t& operator*() const noexcept { return *value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

private:
    bson_t* value_;
};

bool ValidateDepth(const bson_t& document,
                   std::uint32_t depth,
                   std::string& error) {
    if (depth > DocumentCodec::kMaxNestingDepth) {
        error = "document nesting exceeds the limit of " +
                std::to_string(DocumentCodec::kMaxNestingDepth);
        return false;
    }

    bson_iter_t iterator;
    if (!bson_iter_init(&iterator, &document)) {
        error = "unable to iterate BSON document";
        return false;
    }

    std::unordered_set<std::string_view> field_names;

    while (bson_iter_next(&iterator)) {
        const std::string_view field_name(bson_iter_key(&iterator));
        if (field_name.size() > 1024) {
            error = "field name exceeds the 1 KiB limit";
            return false;
        }
        if (!field_names.insert(field_name).second) {
            error = "duplicate field name: " + std::string(field_name);
            return false;
        }
        if (BSON_ITER_HOLDS_DOUBLE(&iterator) &&
            !std::isfinite(bson_iter_double(&iterator))) {
            error = "NaN and Infinity are not supported in documents";
            return false;
        }
        if (!BSON_ITER_HOLDS_DOCUMENT(&iterator) &&
            !BSON_ITER_HOLDS_ARRAY(&iterator)) {
            continue;
        }

        const std::uint8_t* bytes = nullptr;
        std::uint32_t length = 0;
        if (BSON_ITER_HOLDS_DOCUMENT(&iterator)) {
            bson_iter_document(&iterator, &length, &bytes);
        } else {
            bson_iter_array(&iterator, &length, &bytes);
        }

        bson_t child;
        if (!bson_init_static(&child, bytes, length)) {
            error = "invalid nested BSON value";
            return false;
        }
        if (!ValidateDepth(child, depth + 1, error)) return false;
    }
    return true;
}

DocumentCodec::Result DecodeBsonPayload(std::string_view payload) {
    if (payload.size() < 5 ||
        payload.size() > DocumentCodec::kMaxDocumentBytes) {
        return {false, {}, "BSON payload size is outside the supported range"};
    }
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return {false, {}, "BSON payload is too large"};
    }

    bson_t document;
    if (!bson_init_static(
                &document,
                reinterpret_cast<const std::uint8_t*>(payload.data()),
                static_cast<std::uint32_t>(payload.size()))) {
        return {false, {}, "invalid BSON payload"};
    }

    bson_error_t validation_error{};
    if (!bson_validate_with_error(
                &document,
                static_cast<bson_validate_flags_t>(BSON_VALIDATE_UTF8),
                &validation_error)) {
        return {false, {}, "invalid BSON payload: " +
                           std::string(validation_error.message)};
    }

    std::string depth_error;
    if (!ValidateDepth(document, 1, depth_error)) {
        return {false, {}, std::move(depth_error)};
    }

    std::size_t json_length = 0;
    char* raw_json = bson_as_relaxed_extended_json(&document, &json_length);
    if (!raw_json) return {false, {}, "unable to convert BSON to JSON"};

    std::string json;
    json.reserve(json_length);
    bool in_string = false;
    bool escaped = false;
    for (std::size_t i = 0; i < json_length; ++i) {
        const char character = raw_json[i];
        if (in_string) {
            json.push_back(character);
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                in_string = false;
            }
        } else if (character == '"') {
            in_string = true;
            json.push_back(character);
        } else if (character != ' ' && character != '\t' &&
                   character != '\r' && character != '\n') {
            json.push_back(character);
        }
    }
    bson_free(raw_json);
    return {true, std::move(json), {}};
}

} // namespace

DocumentCodec::Format DocumentCodec::Detect(
        std::string_view stored) noexcept {
    if (stored.size() < 4 || stored.substr(0, 3) != "NXD") {
        return Format::LegacyJson;
    }
    if (stored.size() >= kEnvelopeSize &&
        stored.substr(0, 4) == "NXD1" &&
        static_cast<unsigned char>(stored[4]) == 1) {
        return Format::BsonV1;
    }
    return Format::UnknownEnvelope;
}

DocumentCodec::Result DocumentCodec::EncodeJson(std::string_view json) {
    if (json.empty()) return {false, {}, "document must not be empty"};
    if (json.size() > kMaxDocumentBytes) {
        return {false, {}, "JSON document exceeds the 16 MiB limit"};
    }
    if (json.size() > static_cast<std::size_t>(
                              std::numeric_limits<std::int32_t>::max())) {
        return {false, {}, "JSON document is too large"};
    }

    bson_error_t parse_error{};
    OwnedBson document(
            bson_new_from_json(
                    reinterpret_cast<const std::uint8_t*>(json.data()),
                    static_cast<std::int32_t>(json.size()),
                    &parse_error));
    if (!document) {
        return {false, {}, "invalid JSON document: " +
                           std::string(parse_error.message)};
    }
    if (document->len > kMaxDocumentBytes) {
        return {false, {}, "BSON document exceeds the 16 MiB limit"};
    }

    std::string depth_error;
    if (!ValidateDepth(*document, 1, depth_error)) {
        return {false, {}, std::move(depth_error)};
    }

    std::string stored;
    stored.reserve(kEnvelopeSize + document->len);
    stored.append(kBsonEnvelope.data(), kBsonEnvelope.size());
    stored.append(reinterpret_cast<const char*>(bson_get_data(document.get())),
                  document->len);
    return {true, std::move(stored), {}};
}

DocumentCodec::Result DocumentCodec::DecodeToJson(
        std::string_view stored) {
    switch (Detect(stored)) {
        case Format::BsonV1:
            return DecodeBsonPayload(stored.substr(kEnvelopeSize));
        case Format::LegacyJson: {
            // Parse legacy JSON before returning it. This prevents corrupt legacy
            // values from silently entering query/index/update code.
            const Result encoded = EncodeJson(stored);
            if (!encoded.success) return encoded;
            return {true, std::string(stored), {}};
        }
        case Format::UnknownEnvelope:
            return {false, {}, "unsupported Nexora document envelope"};
    }
    return {false, {}, "unknown document format"};
}

} // namespace nexora::core
