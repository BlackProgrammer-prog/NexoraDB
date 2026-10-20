#include "core/DocumentCodec.h"
#include "query/DocumentView.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    if (size > 1024 * 1024) return 0;
    const std::string_view input(reinterpret_cast<const char*>(data), size);

    const auto encoded = nexora::core::DocumentCodec::EncodeJson(input);
    if (encoded.success) {
        const auto decoded = nexora::core::DocumentCodec::DecodeToJson(encoded.value);
        if (!decoded.success) __builtin_trap();
        const std::string_view payload(encoded.value.data() +
                nexora::core::DocumentCodec::kEnvelopeSize,
                encoded.value.size() - nexora::core::DocumentCodec::kEnvelopeSize);
        const nexora::query::DocumentView view(payload);
        if (!view.valid()) __builtin_trap();

        // Derive a bounded field path from the same input. Both valid and invalid
        // paths are useful: Get must always fail safely and never retain input.
        const std::size_t path_size = size > 256 ? 256 : size;
        const nexora::query::CompiledFieldPath path(input.substr(0, path_size));
        (void)view.Get(path);

        const auto reencoded = nexora::core::DocumentCodec::EncodeJson(decoded.value);
        if (!reencoded.success) __builtin_trap();
    } else {
        // Arbitrary bytes may resemble an envelope; decoding must reject them
        // without an out-of-bounds read or exception.
        (void)nexora::core::DocumentCodec::DecodeToJson(input);
    }
    return 0;
}
