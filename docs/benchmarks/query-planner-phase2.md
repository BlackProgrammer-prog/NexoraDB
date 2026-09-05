# Phase 2 Query Planner Benchmark

Date: 2026-09-05  
Dataset: 100,000 deterministic documents, seed `12648430`  
Predicate: equality on `group = "group_42"` (100 matching documents)  
Build: Release, GCC 11.4.0, RocksDB 8.5.3, LTO enabled

| Path | p50 | p95 | p99 | Documents scanned | Index entries scanned |
|---|---:|---:|---:|---:|---:|
| v2 index equality | 0.307 ms | 0.315 ms | 0.316 ms | 100 | 100 |
| full-scan reference | 257.420 ms | 264.152 ms | 266.128 ms | 100,000 | 0 |

The indexed p95 is approximately 839x faster for this workload and reads
1,000x fewer documents. These values are the aggregate of 10 benchmark
repetitions. The complete machine-readable output is stored in
`query_planner_phase2_100k.json`.

Reproduce with:

```bash
cmake --preset benchmark
cmake --build --preset benchmark --parallel
./build/benchmark/benchmarks/cpp/nexora_benchmark \
  '--benchmark_filter=BM_DocumentFindManyEquality.*/documents:100000/manual_time$' \
  --benchmark_min_time=0.05s \
  --benchmark_repetitions=10 \
  --benchmark_report_aggregates_only=true \
  --benchmark_out=docs/benchmarks/query_planner_phase2_100k.json \
  --benchmark_out_format=json
```
