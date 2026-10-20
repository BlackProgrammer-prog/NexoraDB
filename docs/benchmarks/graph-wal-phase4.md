# WAL v2 append/mark scaling

Recorded on 2026-10-19, Linux x86-64 (WSL), GCC 11.4.0, Release + LTO,
Google Benchmark 1.8.2. Each sample preloads the indicated retained history,
then measures 10,000 append-and-mark operations, repeated three times.

| Initial retained records | Mean CPU / append+mark | Mean wall time |
|---:|---:|---:|
| 1,000 | 795 ns | 795 ns |
| 100,000 | 711 ns | 711 ns |
| 1,000,000 | 729 ns | 729 ns |

A 1,000x larger initial history did not increase measured per-operation cost.
The old tree index measured 806 / 1,021 ns at 1,000 / 100,000 retained records
in the earlier run; this is a historical comparison, not a controlled paired run.
The current contiguous-LSN deque provides direct O(1) lookup and amortized O(1)
append and applied-prefix advancement, without a history scan or tree lookup.
This is an algorithmic/scaling claim, not a constant wall-clock latency guarantee:
allocator, scheduler, cache and storage latency still vary (three-run CPU CV was
8.58%, 0.89%, and 1.57%, respectively).
Async policy and a 1 GiB segment limit exclude fsync and rotation to isolate
per-record encoding, write and applied-index work. This does not measure durable
commit latency or end-to-end graph throughput. Setup, prefill and cleanup are
outside the timed benchmark loop. Low initial history grows by 10,000 records
during measurement; the initial-count labels do not imply a fixed-size dataset.

```bash
cmake --preset benchmark
cmake --build build/benchmark --target nexora_benchmark --parallel 2
build/benchmark/benchmarks/cpp/nexora_benchmark \
  --benchmark_filter=BM_WalAppendAndMarkByRetainedRecords \
  --benchmark_repetitions=3
```
