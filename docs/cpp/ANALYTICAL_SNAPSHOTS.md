# Analytical snapshots and algorithm workspaces

Implemented and measured 2026-10-20. No disk/WAL format change.

## Layout and identity contract

`StaticGraph` now builds outgoing CSR directly: row offsets plus one contiguous
AdjEntry array. It does not stage vector-of-vector adjacency or a COO edge copy.
Incoming degrees are computed during construction without an incoming index.
CSC is published once, under a mutex, on the first incoming traversal, or eagerly
if `SnapshotOptions.incoming` is true. Simultaneous first readers are supported;
the resulting indexes are immutable. Both traversal visits both directions;
algorithms requiring unique neighbors, such as Brandes, deduplicate explicitly.

Nodes are **snapshot-local contiguous IDs `0..nodeCount()-1`**, including when the
source has tombstones or filters remove nodes. This intentionally replaces the
old sparse source-ID snapshot contract. Do not pass LiveGraph IDs directly into
snapshot algorithms. Use `snapshot.denseId(external_id)` and
`snapshot.originalId(snapshot_id)` for conversion. Construction uses a bounded
vector remap, not an unordered_map. Public CSR/COO exports retain source IDs in
`originalNodeIds` when remapping, or emit source IDs when remapping is disabled.

Node/edge filters apply before computing degrees. Unknown snapshot type filters
select no matching records. Undirected edges now produce reverse traversal arcs,
with a self-loop counted once per direction. Logical `edgeCount`/COO export count
each record once; CSR exports traversal arcs (two for a non-loop undirected edge).
CSR type IDs are filled in the same slots as their destinations. Raw-ID CSR
allocates rows through the greatest selected original ID, not just active count.

The builder holds LiveGraph's own shared lock across a complete epoch and uses
its private readonly data directly, avoiding recursive public-accessor locks.
Snapshot version, identities, filters and edges therefore come from one epoch.
An implicit-node promotion now advances graph version as well.

## Memory and cancellation

```cpp
SnapshotOptions options;
options.incoming = false;
options.max_build_bytes = 256 * 1024 * 1024;
options.cancellation = std::make_shared<std::atomic<bool>>(false);
auto snapshot = manager.createSnapshot("social", {}, {}, options);
auto job = manager.submitJob("social", algorithm, parameters, options);
job.cancel();
auto result = job.result();
```

`max_build_bytes` combines conservative, overflow-checked preflight with an
enforced PMR requested-allocation limit, **not a process RSS limit**.
It includes source-slot remap, node/edge arrays, identity and
type copies, cursor scratch, and possible CSC headroom. It bounds using source
counts even for selective filters, so it can deliberately reject a filtered
snapshot that would fit with a tighter estimate. `plannedPeakBytes` exposes the
bound; `allocationPeakBytes` and `allocationCurrentBytes` expose tracked requested
bytes (Python: `allocation_peak_bytes`, `allocation_current_bytes`). Every snapshot
container, copied identity/type string, remap, CSC and cursor uses the bounded
resource; allocation is rejected before crossing the limit, including transient
old/new vector buffers. Cache clones charge the retained template as baseline.
Small control objects, root name, allocator overhead/fragmentation, caller-owned
exports and algorithm outputs are excluded. `compressedBytes` exposes actual allocated CSR/CSC capacity only, not the
whole snapshot. Remap/cursor scratch is released before optional CSC construction.
Existing source graph, caller-held snapshots, output export buffers and algorithm
workspaces are separate. Arbitrarily many concurrent caller-owned snapshots/jobs
still require an external aggregate/concurrency budget.

Cancellation checks run throughout construction, edge traversal, BFS/Monte Carlo
iterations and the LPA/centrality computations. Library allocation/copy/sort and
output formatting are not asynchronously interrupted; cancellation is cooperative,
not a hard deadline or thread kill. Custom JobAlgorithms must poll
`snapshot.checkCancelled()` (or use the polling traversal APIs).

Managed submitJob now admits to a bounded pool before building its snapshot;
the snapshot epoch is worker start, not submission time. Direct createSnapshot
remains synchronous. See `JOB_SCHEDULER_AND_BINDINGS.md` for queue/deadline and
resource-reservation semantics. The returned `JobHandle.cancel()` signals queued,
building or running work; an external shared token is optional. Build failure
and cancellation become unsuccessful AlgoResults through submitJob; standalone
constructors throw. The owning algorithm overload pins lifetime; callers of the
legacy borrowed-reference overload must retain their algorithm until completion.

Python exposes SnapshotOptions (`incoming`, `use_cache`, `max_build_bytes`,
`cancel()`), `create_snapshot(..., options=options)`, snapshot `cancel()`,
`has_incoming_index()`, `compressed_bytes()`, `planned_peak_bytes()` and
`original_id()`. Each Python-created options object has a one-shot cancellation
token; sharing it across requests intentionally creates a cancellation group.
Use fresh options for independently cancelable operations.

The heavy builtin C++ helpers and Python `run_connected_components`,
`run_community_detection`, `run_all_distances`, `run_betweenness_centrality`, and
`run_influence_maximization` also accept an optional SnapshotOptions argument.
Python releases the GIL during execution, so a second Python thread can call
`options.cancel()` on the explicitly shared request token while a builtin runs.
For example: `gm.run_all_distances("social", ["", "all"], options=options)`.

## Optional bounded template cache

Caching is **off by default**. With `options.use_cache=true`, GraphManager retains
one immutable CSR template per graph. The key is graph version plus a JSON-encoded,
sorted/deduplicated node/edge filter signature (not ambiguous concatenation).
Version/filter mismatch evicts the previous template before building another.
An uncached request releases that graph's retained template as well.

A hit clones the already remapped/sorted layout into an independent snapshot;
this is not zero-copy shared-buffer caching. It skips source scanning, remap,
degree construction and sorting but still copies arrays/strings/indexes. CSC and
cancellation are request-local. Explicitly shared cancellation tokens remain
shared by caller choice, not by the cache. The conservative clone peak is twice
the template's bound and is checked before the copies. Cached memory is additional
to LiveGraph's memory report; caller-held snapshots are not cache-owned or counted
in the one-entry limit. Cache-hit status is exposed by `cacheHit` / `cache_hit()`.

This policy is useful for repeated stable-version requests; first use pays both
build and clone cost and retains a template. Do not enable it for write-heavy,
one-shot ingestion merely because a warm-cache microbenchmark is faster.

## Algorithm changes

* AllDistances/SSSP uses distance and queue vectors, reused for every source;
  reached-result capacity is reused too. No per-BFS hash nodes/deque growth.
* ConnectedComponents and LPA use direct vector remaps for selected nodes.
* LPA uses reusable label counts and a touched-label vector. Its computational
  per-node loop uses spans, not allocating std::function adapters/hash maps.
* Brandes uses one workspace: queue/BFS order, distances, path counts, deltas,
  predecessor offsets/counts/storage, and neighbor stamps. Predecessor storage is
  reserved from total incoming/outgoing degree before source/node loops. Optional
  CSC is built before those loops. No per-node unordered_set or predecessor-vector
  growth, no per-source copied stack/delta allocation. It retains the previous
  undirected/Both interpretation and deduplicates parallel neighbor entries.
* InfluenceMaximization uses vector adjacency/seed/activation bitmaps and one
  reusable Monte Carlo queue. Fixed seeding remains reproducible, but ascending
  bitmap iteration changes the old unordered-container RNG draw order; stochastic
  results are not promised byte-identical to the previous implementation.
* FriendSuggestion, centrality top-k and the influence candidate shortlist use
  partial_sort; MostConnected already did. Limited prefixes preserve ranking/ties.

These guarantees concern computation, not inevitable allocations for returned
JSON/member lists. Brandes still has quadratic all-source work, and all-pairs
output can itself be quadratic; buffer reuse does not change those complexities.

## Release probe and parallelization gate

```sh
cmake --preset benchmark -DBUILD_TESTING=ON
cmake --build build/benchmark --target nexora_graph_snapshot_probe --parallel 2
build/benchmark/tests/cpp/nexora_graph_snapshot_probe
```

Linux x86-64 / WSL, GCC 11.4, Release + LTO. Directed ring, four outgoing arcs per
node. An idle-build run on 2026-10-20 measured:

| Nodes | CSR buffers | outgoing vector-list fixture | initial full CSR snapshot | lazy CSC | five rebuilds | five cached-layout clones |
|---:|---:|---:|---:|---:|---:|---:|
| 1,000 | 104,008 B | 120,000 B | 0.075 ms | 0.013 ms | 0.402 ms | 0.153 ms |
| 10,000 | 1,040,008 B | 1,200,000 B | 1.448 ms | 0.151 ms | 5.779 ms | 2.470 ms |

The list fixture measures outgoing adjacency only, not the old full snapshot;
its staging time is not comparable to the new full constructor. CSR+CSC buffers
double these CSR-only byte counts for this fixture. Build bounds were 2,324,036 /
23,276,036 B and are intentionally not measured peak RSS. The clone comparison
justifies an opt-in template cache, not a default assumption of application reuse.

For 32 BFS sources, median wall time over three passes:

| Nodes | per-source hash BFS | reused vector BFS | two independent worker workspaces |
|---:|---:|---:|---:|
| 1,000 | 1.057 ms | 0.082 ms | 0.132 ms |
| 10,000 | 10.441 ms | 0.784 ms | 0.499 ms |

All checksums matched. Two workers regress the small fixture; a candidate lower
gate is **at least 10,000 nodes and 32 independent sources**, plus enough room for
two workspaces. This is a benchmark observation, not a universal threshold.
Production algorithms deliberately remain serial: enabling automatic parallelism
still requires degree-skew/real-topology tests, aggregate workspace budgets and
cancellation/output-order validation. No hidden thread pool or unsafe default
parallelization was added. The requested benchmark/threshold gate is documented;
parallel execution itself is not claimed implemented.

## Tests

Nine new tests cover holes/dense IDs, filtered degree/type-aligned exports,
undirected/self-loop arcs, lazy CSC concurrent publication, concurrent source
writers, cancellation/preflight, bounded cache version/filter/token behavior,
path-graph centrality reference, all-source BFS distances, LPA/Monte Carlo
workspaces, JobHandle cancellation/error propagation, and top-k/full-prefix parity.
