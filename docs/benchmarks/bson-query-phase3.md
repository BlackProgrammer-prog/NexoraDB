# BSON query evaluator microbenchmark

This microbenchmark isolates three compiled predicates over one document. It
compares the production BSON `DocumentView` path with the JSON compatibility
facade, which must parse JSON and construct a temporary BSON document.

## Reproduce

```bash
cmake --preset benchmark
cmake --build --preset benchmark --parallel
./build/benchmark/benchmarks/cpp/nexora_benchmark \
  --benchmark_filter='BM_.*Predicate' \
  --benchmark_min_time=0.02s
```

## Initial result

Recorded on 2026-09-06 with GCC 11.4.0, Release + LTO, RocksDB 8.5.3 and
libbson 1.24.3:

| Workload | CPU time | Relative throughput |
|---|---:|---:|
| `BM_CompiledBsonPredicate` | 135 ns | 7.39 M docs/s |
| `BM_JsonCompatibilityPredicate` | 1,575 ns | 0.635 M docs/s |

The direct BSON path was approximately **11.7x faster** in this isolated run.
This is a microbenchmark, not an end-to-end latency claim. Re-run it on the
target production hardware and retain the JSON output before release decisions.
