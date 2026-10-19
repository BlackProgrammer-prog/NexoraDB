# Bulk graph ingestion

`graph/BulkGraphBuilder.h` exposes a synchronous, single-producer C++ builder.
`GraphManager::buildGraph` uses it for node mappings, edge mappings and UNWIND;
incremental document notifications keep using ordinary CRUD. Reported build
counts now describe actual active nodes/edges, including implicit nodes and
excluding duplicate edges, rather than documents visited.

```cpp
BulkOptions limits;
limits.max_records = 4096;
limits.max_buffer_bytes = 8 * 1024 * 1024;
BulkGraphBuilder builder(graph, limits);
builder.addNode("alice", "User");
builder.addEdge("alice", "bob", "KNOWS", false);
builder.flush(); // Required: destructor never performs I/O or commits.
```

## Batch pipeline

1. Admission checks a record's size before copying. If the next record exceeds
   remaining bytes or record count, synchronously flush the previous batch.
   The producer cannot outrun persistence; there is no unbounded background queue.
   Empty identifiers and a record too large to fit fail before admission.
2. Acquire the LiveGraph writer lock once. Resolve explicit nodes before implicit
   endpoints; explicit declarations promote implicit nodes. Repeated explicit
   declarations preserve the first existing explicit type, matching CRUD.
3. Canonicalize undirected endpoints, sort/dedup candidate edges, and compare with
   existing adjacency. Identity is endpoint/type adjacency, not a parallel-edge
   multigraph. Existing edges retain their direction. Within a batch, undirected
   candidates win conflicting directed inputs; reverse directed edges remain
   distinct if no undirected candidate occupies those arcs. Different edge types
   remain distinct. Self-loops contribute one incoming and one outgoing entry.
4. Accumulate final degrees in RAM. Persist identities/types in one metadata
   replacement before the WAL. Append redo records under one WAL lock with one
   batch durability barrier (segment rotation may require additional barriers).
   There is no per-record lock or fsync in this append path.
5. Reserve final adjacency degree, append entries without sorted insertion, then
   sort each touched adjacency. Heavy chunked lists are flattened once into bulk
   vectors; later incremental mutations can upgrade them again.
6. Under one storage lock, write every touched node once with its final degrees,
   then new edges; flush streams once per file, not once per degree update.
   Contiguous writes avoid intervening seeks. Sync data before one applied-prefix
   checkpoint for Sync/GroupCommit. Async retains its documented weaker contract;
   use `graph.checkpointWAL()` to establish an explicit durable checkpoint.

## Limits and failure semantics

The buffer budget is a conservative **logical ingestion charge**,
`1024 + 4 * input string bytes` per node/edge, plus a hard record-count cap.
`bufferedBytes()` and `stats().peak_buffer_bytes` report that charge, not measured
RSS. It bounds queued ingestion and batch-sized candidate/redo work. It does not
cap the resident graph, temporary flattening of existing heavy adjacency, JSON
metadata serialization, source-document decoding/UNWIND arrays, allocator overhead
or retained WAL memory. This is not an external-memory graph builder or a hard
process-memory limit; those require a separate spill/streaming architecture.

Batching is **not transaction atomicity**. A failed/crashed append may leave a
valid redo prefix, and preceding batches remain committed. Operations reference
stable slot IDs and replay idempotently. The order is metadata -> WAL durability
-> node/edge data -> data durability -> checkpoint. Startup remains fail-closed
on complete-record corruption or torn graph records; the tests do not establish
recovery from arbitrary torn data-file writes or physical power failure.

An exception during flush poisons the builder and graph writer. Destroy/recreate
the graph, reopen storage/WAL, load and replay before retrying ingestion. Do not
use partially updated RAM after failure. Destructor discards only unflushed input,
not already committed batches. Concurrent builders serialize complete batches on
the graph lock; a single builder itself must not be used concurrently.

## Validation

`tests/cpp/test_graph_bulk.cpp` covers sorted dedup within/across batches, mixed
direction collisions, self-loops, implicit promotion, persisted degrees, bounded
admission and oversized records, existing heavy adjacency, all durability modes,
segment rotation and deleted-slot reuse, poisoned writers, and production BSON
mapping/UNWIND integration. A batch without rotation performs one WAL sync,
verified with `syncCount()`.

Actual SIGKILL at the post-WAL, post-data-sync and post-checkpoint boundaries,
followed by repeated restarts, verifies counts, traversal and idempotent replay.
These are POSIX process-crash tests, not exhaustive mid-write/power-cut tests.
