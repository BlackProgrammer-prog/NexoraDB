# Phase 3 parser and allocation benchmark

The Phase 3 acceptance benchmark compares the old JSON compatibility path with
the production compiled BSON predicate path. A process-wide `operator new`
counter records allocation count and requested bytes only across the measured
loop; setup, compilation and warm-up are excluded.

## Reproduce

```bash
cmake --preset benchmark
cmake --build --preset benchmark --parallel
build/benchmark/benchmarks/cpp/nexora_phase3_allocation_benchmark 100000
```

## Result

Recorded on 2026-10-19 with GCC 11.4.0, Release + LTO, RocksDB 8.5.3 and
libbson 1.24.3:

| Path | allocations/document | allocated bytes/document | CPU time/document |
|---|---:|---:|---:|
| Compiled BSON | 0 | 0 | 128 ns |
| JSON compatibility | 12 | 2,224 | 1,463 ns |

For this three-predicate document workload, the canonical compiled BSON path
eliminated measured per-document heap allocation and was about 11.4x faster.
The byte counter reports requested allocation sizes, not allocator metadata or
peak RSS. Results are microbenchmarks and should be regenerated on production
hardware before making end-to-end latency claims.
