#include "core/DocumentCodec.h"
#include "query/CompiledQuery.h"
#include "query/Condition.h"
#include "query/DocumentView.h"
#include "query/Evaluator.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <string_view>

namespace {
std::atomic<std::uint64_t> allocation_count{0};
std::atomic<std::uint64_t> allocation_bytes{0};
}

void* operator new(std::size_t size) {
    if (void* memory = std::malloc(size)) {
        allocation_count.fetch_add(1, std::memory_order_relaxed);
        allocation_bytes.fetch_add(size, std::memory_order_relaxed);
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    void* memory = nullptr;
    if (posix_memalign(&memory, static_cast<std::size_t>(alignment), size) != 0)
        throw std::bad_alloc();
    allocation_count.fetch_add(1, std::memory_order_relaxed);
    allocation_bytes.fetch_add(size, std::memory_order_relaxed);
    return memory;
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}
void operator delete(void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
    std::free(memory);
}
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
    std::free(memory);
}

namespace {

using Clock = std::chrono::steady_clock;
using nexora::query::Condition;
using nexora::query::Op;
using nexora::query::ValueType;

struct Measurement {
    std::uint64_t allocations;
    std::uint64_t bytes;
    double nanoseconds;
};

template <typename Function>
Measurement measure(std::size_t iterations, Function&& function) {
    for (std::size_t index = 0; index < 1000; ++index) function();
    const auto allocations_before = allocation_count.load(std::memory_order_relaxed);
    const auto bytes_before = allocation_bytes.load(std::memory_order_relaxed);
    const auto begin = Clock::now();
    for (std::size_t index = 0; index < iterations; ++index) function();
    const auto end = Clock::now();
    return {
        allocation_count.load(std::memory_order_relaxed) - allocations_before,
        allocation_bytes.load(std::memory_order_relaxed) - bytes_before,
        std::chrono::duration<double, std::nano>(end - begin).count()
    };
}

void print(std::string_view name, const Measurement& value,
           std::size_t iterations) {
    std::cout << name << ','
              << static_cast<double>(value.allocations) / iterations << ','
              << static_cast<double>(value.bytes) / iterations << ','
              << value.nanoseconds / iterations << '\n';
}

} // namespace

int main(int argc, char** argv) {
    const std::size_t iterations = argc == 2
            ? static_cast<std::size_t>(std::stoull(argv[1]))
            : 100000;
    const std::string json =
            R"({"_id":"bench","profile":{"age":42,"name":"Alice"},"active":true})";
    const Condition condition = Condition::And({
            Condition::Leaf("profile.age", Op::GTE, "40", ValueType::Int64),
            Condition::Leaf("profile.name", Op::STARTS, "Ali"),
            Condition::Leaf("active", Op::EQ, "true", ValueType::Bool)});
    const auto encoded = nexora::core::DocumentCodec::EncodeJson(json);
    if (!encoded.success) {
        std::cerr << encoded.error << '\n';
        return 1;
    }
    const nexora::query::DocumentView view(
            std::string_view(encoded.value).substr(
                    nexora::core::DocumentCodec::kEnvelopeSize));
    const nexora::query::CompiledQuery compiled(condition);
    nexora::query::Evaluator compatibility;
    if (!view.valid() || !compiled.valid()) return 2;

    bool result = false;
    const auto bson = measure(iterations, [&] {
        result ^= compiled.Match(view);
    });
    const auto legacy = measure(iterations, [&] {
        result ^= compatibility.Match(json, condition);
    });
    std::cout << "path,allocations_per_document,allocated_bytes_per_document,ns_per_document\n";
    print("compiled_bson", bson, iterations);
    print("json_compatibility", legacy, iterations);
    return result ? 0 : 0;
}
