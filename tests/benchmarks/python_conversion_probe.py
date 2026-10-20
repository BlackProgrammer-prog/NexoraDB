"""Boundary microbenchmark: includes pybind11 dispatch/allocation/conversion.

No database I/O. Set PYTHONPATH to build/<preset>/tests/cpp; report the preset
alongside results. These timings do not isolate conversion from native allocation.
"""
import json
import statistics
import time
import nexora_conversion_probe as native


def median_us(call, repeat):
    samples = []
    for _ in range(7):
        start = time.perf_counter_ns()
        for _ in range(repeat):
            call()
        samples.append((time.perf_counter_ns() - start) / repeat / 1000)
    return statistics.median(samples)


results = []
for size in (16, 1024, 1024 * 1024):
    value = "x" * size
    repeat = 500 if size < 10000 else 30
    assert native.roundtrip_string(value) == value
    results.append({"bytes": size,
                    "roundtrip_us": median_us(lambda: native.roundtrip_string(value), repeat),
                    "native_alloc_and_output_us": median_us(lambda: native.make_string(size), repeat)})
for count in (100, 10000):
    values = ["x" * 32] * count
    assert native.sum_strings(values) == count * 32
    results.append({"strings": count, "width": 32,
                    "python_to_cpp_vector_us": median_us(lambda: native.sum_strings(values), 30),
                    "cpp_to_python_list_us": median_us(lambda: native.make_strings(count, 32), 30)})
print(json.dumps(results, indent=2))
