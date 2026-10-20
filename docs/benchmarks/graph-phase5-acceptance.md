# Phase 5 acceptance — 2026-10-20

Linux x86-64 / WSL Ubuntu, GCC 11.4, Release + LTO for probes.
These are reproducible fixtures, not universal performance guarantees.

## Correctness and integration

`GraphPhase5.BulkMatchesSingleForRandomCorpusAndEveryBatchBoundary` compares
external identities, explicit/implicit types, degrees, logical typed/directed
edges and incoming/outgoing traversal against sequential CRUD. It covers a
seeded random corpus, mixed-direction conflicts, duplicates, self-loops,
Unicode/escaped IDs and batch sizes 1, 2, 7, 64, 4096. Internal DenseId/EdgeId
assignment is intentionally not an equivalence requirement.

The mixed-direction test exposed different bulk/single conflict precedence.
Both now preserve the first accepted input, including reverse-direction
conflicts. A second test compares three opens with Sync persistence and Async
redo. Existing SIGKILL batch tests remain part of the acceptance suite.

`GraphManager::buildGraph` uses BulkGraphBuilder for node/edge mappings and
UNWIND. Incremental notifications deliberately retain CRUD. Dense edge storage
uses vector/tombstones with a sparse-ID fallback, reverse identity uses a vector,
and analytical traversal uses CSR with lazy CSC. String-to-ID lookup remains a
hash map; not every data structure should be dense.

## Allocated memory and RSS

`nexora_graph_phase5_memory_probe` instruments global allocation **only in its
standalone executable**, never in the production library. Live requested bytes
include capacity, container nodes, strings and transient allocation peaks.
Malloc headers/fragmentation are not requested payload. `maxrss_kib` separately
reports Linux process high-water RSS, including the instrumentation's own
headers; it is not an isolated graph RSS measurement.

Fixture: 100,000 short-ID isolated nodes, then 50,000 disjoint directed edges.
The baseline reconstructs the former layout (104-byte adjacency objects,
forward/reverse hash maps and edge hash map), not a historical whole-engine
binary. The current side runs real LiveGraph and includes its scaffolding.
Both use ordinary geometric container growth, no special baseline reservation.
Each row runs in a fresh process.

| Metric | Legacy layout fixture | Current LiveGraph |
|---|---:|---:|
| Isolated requested bytes | 30,992,936 | 27,957,240 |
| Isolated bytes/node | 309.929 | 279.572 |
| Paired requested bytes | 36,474,768 | 30,054,704 |
| Marginal bytes/edge | 109.637 | 41.949 |
| Requested allocation peak | 36,474,768 | 31,103,280 |
| Process peak RSS, KiB | 52,492 | 34,416 |

Paired requested payload falls **17.6%**, isolated payload **9.8%**, and marginal
edge payload **61.7%** in this low-degree fixture. RSS is supporting evidence,
not a clean old/new engine comparison or a general budget guarantee.

## Snapshot budget contract

All snapshot containers, copied node/type strings, remap, row/cursor scratch and
lazy CSC use a bounded PMR resource. Each request is checked before upstream
allocation; reallocation charges old and new buffers while both exist. Cache
clones charge the retained template as a baseline. Conservative preflight may
reject earlier than the measured allocator cap. Zero means unlimited.

`allocationPeakBytes()` / Python `allocation_peak_bytes()` report measured
requested-byte peak. The budget excludes small control objects/root name,
allocator metadata, source graph, unrelated snapshots, exported buffers and
algorithm workspaces. **It is not a hard RSS/process-memory limit.** Application
concurrency still needs an aggregate budget. The template baseline assumes the
immutable cache template; public concurrent growth of unrelated source indexes
is not globally accounted by the clone resource.

Tests cover exact 128-byte admission, failure before crossing the limit,
transient vector growth, long identities, lazy CSC, clone baseline and cleanup.

Measured directed ring snapshots (four outgoing arcs/node), including lazy CSC
scratch, with a 67,108,864-byte requested-allocation budget:

| Nodes | Measured peak requested bytes | Retained requested bytes |
|---:|---:|---:|
| 1,000 | 489,216 | 481,208 |
| 10,000 | 4,882,528 | 4,802,520 |

Both stayed below the configured budget. The 128-byte failure test establishes
admission behavior; these larger measurements alone do not prove enforcement.

## End-to-end ingestion and WAL history

Fixture: 200 explicit nodes, 600 unique directed ring edges, 800 inputs total,
batch limit 128. Includes graph/storage/WAL writes and metadata replacement.
Each sample uses fresh temporary files. Retained history is synthetic applied
Commit control records (no fabricated graph mutations). Prefill, reopen/recovery,
setup and final Async checkpoint are outside the timer. Async is **not** durable
acknowledgement latency. Sync includes its normal per-operation durability.
All samples verified 200 nodes, 600 edges and 800 new WAL records; semantic
equivalence is independently tested above. Segment limit is 1 GiB, intentionally
excluding rotation from this scaling test. No build/test ran during this run.

Median of three repeats, alternating single/bulk order:

| Policy | Retained records | Single ms | Bulk ms | Bulk inputs/s |
|---|---:|---:|---:|---:|
| Async | 1,000 | 881.552 | 29.067 | 27,523 |
| Async | 100,000 | 866.255 | 28.239 | 28,330 |
| Async | 1,000,000 | 1,016.510 | 32.322 | 24,751 |
| Sync | 0 | 8,011.640 | 88.861 | 9,003 |

Bulk is about **30x** faster in the first Async fixture and **90x** faster in
the Sync fixture. A 1,000x larger retained history changes median bulk time by
1.11x, not 1,000x; this supports absence of history-length-linear ingestion
degradation in this range. Individual Async bulk times span 26.055–47.497 ms,
and Sync bulk spans 87.554–102.561 ms. Storage/OS variability remains; no exact
constant-time latency or extrapolation to arbitrary WAL sizes is promised.

## Reproduction

```sh
cmake --preset benchmark -DBUILD_TESTING=ON
cmake --build build/benchmark --target nexora_graph_phase5_memory_probe nexora_graph_phase5_ingestion_probe nexora_graph_snapshot_probe --parallel 2
build/benchmark/tests/cpp/nexora_graph_phase5_memory_probe legacy
build/benchmark/tests/cpp/nexora_graph_phase5_memory_probe dense
build/benchmark/tests/cpp/nexora_graph_phase5_ingestion_probe
build/benchmark/tests/cpp/nexora_graph_snapshot_probe
ctest --test-dir build/debug --output-on-failure --parallel 2
```

Full Debug CTest: **105/105**. Graph suite with ASan + leak detection:
**69/69**; with UBSan halt-on-error: **69/69**. Debug Python bindings build.
Timing measurements are observational probes, not flaky CI timing assertions.
