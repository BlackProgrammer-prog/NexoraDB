# Graph memory layout and admission

Implemented and measured on 2026-10-20. Disk v2, WAL records and public DenseId /
EdgeId values are unchanged; all changes below concern runtime representation.

## Representation

* `EdgeStore` keeps contiguous edge slots in `vector<EdgeRecord>`; flags encode
  inactive/tombstone slots and iteration skips them. Dense slots store no separate
  hash key/node. Sparse outliers go into a fallback hash map, so an ID near
  UINT64_MAX cannot trigger a vector of that length. Restart/compaction pack the
  fallback into slots only when the largest ID fits a bounded density threshold
  (`max(4096, 2 * active_count)`). IDs are not remapped by this optimization.
  Tombstone capacity can remain after deletions; compaction reclaims logical IDs.
* DenseId-to-ExtId uses `vector<optional<string>>`. Optional occupancy preserves
  empty external strings, holes and reuse. Dense IDs follow the same bounded-file
  slot contract as node/adjacency vectors; this is not a general sparse-ID map.
  ExtId-to-DenseId remains an owning string hash map (decision below).
* Normal adjacency uses `SmallAdjVector<AdjEntry>`: degree zero/one is inline;
  degree two or higher spills to a geometric-growth vector. Removal to degree
  one releases its heap buffer. Heavy chunked state is allocated lazily, not
  embedded in every node. Deletion below half the heavy threshold downgrades with
  hysteresis and frees chunked state, eventually returning to inline storage.
  Upgrade releases the normal buffer; bulk flattening
  releases the heavy representation. NodeAdj copy is deep; moves leave a usable
  source. Large-list sorting and reserve semantics remain available to bulk load.

### Edge duplication audit

Each active edge has exactly one canonical mutable EdgeRecord in EdgeStore, not
both a slot record and a second record/key hash index. Sparse fallback keys are a
deliberate exception for non-dense IDs. Adjacency retains only neighbor, edge ID
and type as traversal indexes; it does not duplicate complete EdgeRecords or
source BSON. The two indexes are necessary for incoming/outgoing traversal, and
undirected edges require reverse entries. WAL redo and immutable StaticGraph
snapshots are separate required lifetime/consistency copies, not removed caches.
Reporting exposes retained WAL memory rather than pretending it is free.

## Memory API

```cpp
auto usage = graph.memoryUsage();
graph.setMemoryBudget(256 * 1024 * 1024); // conservative admission bytes
graph.setMemoryBudget(0);                // disable
```

Python uses `gm.memory_usage(graph_name)` and
`gm.set_memory_budget(graph_name, bytes)`. Limits are runtime-only, not persisted.

Breakdown includes graph object, node-record capacity, adjacency header capacity,
adjacency vector/chunk capacity, canonical edge slots plus sparse fallback,
reverse identities, forward identities, type registries, free-ID stacks,
retained WAL, and storage metadata. `total_bytes_estimate` sums all components.
Dynamic string capacity is included when not inline.

Vector payload/capacity sizes and `sizeof` are exact requested storage sizes.
Hash node/link overhead, free-stack backing blocks, WAL deque blocks and JSON
tree overhead remain implementation-dependent estimates. Stream buffers,
filesystem path caches, allocator headers/fragmentation, temporary compaction /
replay / bulk workspaces, independent snapshots and RocksDB are not fully counted.
**This API is not RSS and is not a hard allocator/process-memory cap.**

### Admission contract

Enabling a budget first checks current estimated resident size. Future normal
CRUD and bulk ingestion reserve conservative credits before changing IDs,
registries, graph data, WAL or storage: 1024 bytes per potentially new node,
512 per candidate edge and four times input string bytes. Bulk admits its whole
batch before mutating anything, including worst-case implicit endpoints.
Rejection throws `length_error`; buffered bulk input remains available for retry
after increasing the budget. It does not poison an otherwise healthy writer.
Existing duplicate edges may consume conservative credits; deletes do not refund
credits because capacities can remain. `admission_charged_bytes` is a high-water
admission ledger, not measured allocated bytes. Explicitly setting the budget
again rebaselines from the current estimate.

Budget checks apply to ingestion, not startup loading, replay or compaction; these
must be able to recover already accepted durable operations. Configure the limit
after loading. Use an OS/container limit for a hard whole-process cap, with room
for recovery/transient work. The existing BulkOptions buffer budget is separate.

## Reproducible measurements

`tests/cpp/graph_memory_probe.cpp` measures `sizeof`, current allocator-requested
bytes for baseline STL hash maps, actual vector capacity, and a flat prototype.
SSO IDs (`n0`, `n1`, ...) avoid uninstrumented string allocations in the baseline.
It is an evaluation executable, not a timing assertion in CTest.

```sh
cmake --preset benchmark -DBUILD_TESTING=ON
cmake --build build/benchmark --target nexora_graph_memory_probe --parallel 2
build/benchmark/tests/cpp/nexora_graph_memory_probe
```

Linux x86-64 / WSL, GCC 11.4, Release + LTO, libstdc++:

| Item | Before | After |
|---|---:|---:|
| NodeRecord / EdgeRecord / AdjEntry | 32 / 32 / 24 B | unchanged |
| NodeAdj object | 104 B | 88 B |
| One-entry normal adjacency payload | 24 B minimum heap request | 0 B heap |
| Reverse identity occupied slot | hash node/key + string | 40 B optional string slot |
| 10,000 canonical dense edges | 562,184 B hash allocator requests | 320,000 B slot buffer |

The edge comparison is 43.1% less requested storage for this fixture. The slot
buffer was packed once after randomized insertion; geometric capacity on a growing
graph can be larger. These numbers exclude container objects and malloc metadata;
they are not whole-process savings. NodeAdj alone is 15.4% smaller; most low-degree
nodes additionally avoid one or two separate heap allocations.

For a 10,000-node in-memory fixture, the report estimated 3,264,760 B while isolated
and 3,527,208 B after 5,000 disjoint directed edges. Adjacency heap payload stayed
zero in both, because each direction had degree at most one. Report estimates
include capacity slack and must not be confused with allocator-request counters.

## ExtId flat/Swiss evaluation and decision

A local linear-probing prototype with a 70% maximum load and power-of-two slots
was compared with the current standard owning-string map. A single Release run
used 20 successful-lookup passes per fixture:

| IDs | std hash requested bytes | flat prototype buffer | std lookup ns | flat lookup ns |
|---:|---:|---:|---:|---:|
| 1,000 | 64,872 | 98,304 | 7.93 | 7.26 |
| 10,000 | 642,184 | 786,432 | 14.58 | 14.09 |
| 100,000 | 6,983,464 | 12,582,912 | 23.60 | 24.25 |

The prototype is **not SwissTable**, not production-ready, and does not test
deletion, misses, hostile hashes, long IDs or churn. Timings have no confidence
interval and establish no general performance ranking. Its memory result does
not justify replacing the production map with this prototype.

Swiss was also reviewed as a library candidate: Abseil stores values in its slot
array; rehash invalidates element pointers/references, and its documented space
model is roughly `(sizeof(value_type)+1)*bucket_count`. At this ABI a 40-byte
string/ID pair implies about 46.9–93.7 bytes per entry over the documented usual
load range, excluding separately allocated string characters. This is a model,
not an Abseil benchmark. See the primary [Abseil container guide](https://abseil.io/docs/cpp/guides/container).

Decision: retain std::unordered_map for this change; Abseil is not a current
manifest dependency. No claim is made that std beats Swiss. A production Swiss
migration remains optional follow-up, contingent on a pinned dependency and an
actual long-string/delete/churn benchmark plus correctness validation. The
requested evaluation is complete; adoption was not assumed from its name.

## Validation

Six new tests cover randomized EdgeStore/map equivalence, sparse-ID boundedness,
tombstone iteration/reuse, reverse identity holes/empty strings, inline/spill /
heavy transitions and deep copy, budget rejection before WAL/data mutation, and
breakdown/restart consistency. Existing CRUD, bulk, compaction and SIGKILL tests
exercise the new representation without any disk-format change.
