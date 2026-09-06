#ifndef NEXORADB_DOCUMENT_CODEC_H
#define NEXORADB_DOCUMENT_CODEC_H

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace nexora::core {

/**
 * Versioned physical document codec.
 *
 * Public APIs continue to accept and return UTF-8 JSON. RocksDB values use an
 * eight-byte Nexora envelope followed by a standards-compliant BSON document:
 *
 *   bytes 0..3  "NXD1" (envelope version)
 *   byte  4     payload codec (1 = BSON)
 *   byte  5     flags (reserved, zero)
 *   bytes 6..7  reserved (zero)
 *   bytes 8..   BSON bytes, including BSON's trailing NUL
 */
class DocumentCodec final {
public:
    static constexpr std::size_t kEnvelopeSize = 8;
    static constexpr std::size_t kMaxDocumentBytes = 16U * 1024U * 1024U;
    static constexpr std::uint32_t kMaxNestingDepth = 100;

    enum class Format : std::uint8_t {
        LegacyJson = 0,
        BsonV1 = 1,
        UnknownEnvelope = 255
    };

    struct Result {
        bool success = false;
        std::string value;
        std::string error;
    };

    [[nodiscard]] static Format Detect(std::string_view stored) noexcept;
    [[nodiscard]] static Result EncodeJson(std::string_view json);
    [[nodiscard]] static Result DecodeToJson(std::string_view stored);

private:
    DocumentCodec() = delete;
};

} // namespace nexora::core

#endif // NEXORADB_DOCUMENT_CODEC_H
