#include "query/CompiledUpdate.h"
#include "query/UpdateSpec.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    if (size < 2 || size > 1024 * 1024) return 0;
    const std::size_t split = 1 + (data[0] % (size - 1));
    const std::string document(reinterpret_cast<const char*>(data + split),
                               size - split);
    static constexpr std::array<nexora::query::UpdateOp, 14> operations{
        nexora::query::UpdateOp::Set, nexora::query::UpdateOp::Unset,
        nexora::query::UpdateOp::Inc, nexora::query::UpdateOp::Mul,
        nexora::query::UpdateOp::Min, nexora::query::UpdateOp::Max,
        nexora::query::UpdateOp::Rename, nexora::query::UpdateOp::CurrentDate,
        nexora::query::UpdateOp::Push, nexora::query::UpdateOp::Pull,
        nexora::query::UpdateOp::AddToSet, nexora::query::UpdateOp::PushAll,
        nexora::query::UpdateOp::PullAll, nexora::query::UpdateOp::Pop};
    static constexpr std::array<nexora::query::UpdateValueType, 7> types{
        nexora::query::UpdateValueType::String,
        nexora::query::UpdateValueType::Int64,
        nexora::query::UpdateValueType::Float64,
        nexora::query::UpdateValueType::Bool,
        nexora::query::UpdateValueType::Null,
        nexora::query::UpdateValueType::Array,
        nexora::query::UpdateValueType::Object};

    nexora::query::UpdateOperation operation;
    operation.op = operations[data[0] % operations.size()];
    operation.field = (data[0] & 0x80U) ? "profile.value" : "value";
    const std::size_t value_size = split > 1 ? split - 1 : 0;
    operation.value.assign(reinterpret_cast<const char*>(data + 1), value_size);
    operation.value_type = types[(split < size ? data[split] : data[0]) % types.size()];
    if (operation.op == nexora::query::UpdateOp::Rename)
        operation.value = (data[0] & 1U) ? "renamed" : "profile.renamed";
    if (operation.op == nexora::query::UpdateOp::PushAll ||
        operation.op == nexora::query::UpdateOp::PullAll) {
        operation.values = {operation.value, operation.value};
    }

    nexora::query::UpdateSpec spec;
    spec.Add(std::move(operation));
    const nexora::query::CompiledUpdate compiled(spec);
    if (!compiled.valid()) return 0;
    const auto result = compiled.ApplyJson(document);
    if (result.success) {
        const auto parsed = nlohmann::ordered_json::parse(
                result.document, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) __builtin_trap();
    }
    return 0;
}
