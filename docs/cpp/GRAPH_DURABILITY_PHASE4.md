# Phase 4: graph WAL v2

Implemented on 2026-10-19. The **new WAL** checklist is complete. Graph storage
format and compaction are described in `GRAPH_STORAGE_V2.md`. Source-document
to-graph reconciliation remains separate, open work.

## Files and encoding

The first segment is `graph.wal`. Subsequent segments are
`graph.wal.segment.<20-digit-first-LSN>`. Records are immutable 72-byte values,
encoded explicitly in little endian, independently of C++ struct layout:

| Offset | Bytes | Value |
|---:|---:|---|
| 0 | 4 | `NXW2` magic |
| 4 | 4 | Complete record length, 72 |
| 8 | 4 | Format version, 2 |
| 12 | 4 | IEEE CRC32 of the complete record with this slot zeroed |
| 16 | 8 | LSN, starting at 1 |
| 24 | 8 | Unix timestamp in milliseconds |
| 32 | 4 | Operation code |
| 36 | 4 | Record flags, including undirected edge flag |
| 40 | 8 | Node/edge ID |
| 48 | 8 | Source node ID |
| 56 | 8 | Destination node ID |
| 64 | 4 | Type ID |
| 68 | 4 | Reserved, zero |

`graph.wal.checkpoint` is a separate 24-byte value: `NXC2` (4), version 2 (4),
contiguous applied LSN (8), CRC32 (4), reserved zero (4). The CRC is computed with
its own slot zeroed. Checkpoints use an exclusive temporary file, fsync, rename,
and directory fsync. A process-owned `flock` on `graph.wal.lock` prevents a second
writer from acquiring the same WAL.

`markApplied` changes memory and the checkpoint, never an existing record.
Out-of-order markers cannot advance the checkpoint past an unapplied record.
Contiguous retained LSNs use a deque with direct O(1) sequence indexing and
amortized O(1) append; advancing the applied prefix visits each record once.
Prefix retention pops records without reindexing the remaining history.
Startup, replay, and diagnostic queries
can scan retained records. No per-mutation mark operation scans the WAL file.
The index and retained decoded records use O(retained records) memory.

Acceptance validation (2026-10-19): directed and undirected edges survive actual
SIGKILL after durable WAL append, after durable data writes, and after checkpoint
publication. Three subsequent restarts verify counts, adjacency, stored degrees
and replay idempotence. Tail tests cover all 71 incomplete record lengths,
checkpoint-covered torn records, sealed segments, and full-record corruption.
Only an unacknowledged incomplete active tail is truncated and synced; a complete
record with a bad CRC fails closed without discarding bytes. These are POSIX
process-crash tests, not physical power-loss tests. Scaling measurements through
one million retained records are in `../benchmarks/graph-wal-phase4.md`.

## Durability contract

For `Sync` (default) and `GroupCommit`, a graph operation follows this order:

1. Encode and append redo bytes to the active WAL segment.
2. Complete WAL `fdatasync` before any corresponding graph data write.
3. Write graph data; flush the C++ streams and `fdatasync` node, edge and metadata
   files, then fsync the graph directory.
4. Mark the LSN applied and publish the contiguous checkpoint durably.
5. Return success to the caller.

Disk errors propagate as operation failures. A failed WAL enters a failed state:
further appends/checkpoints and retention are rejected until close/reopen.
Checkpoint failure after rename can have an ambiguous commit outcome; retry
after recovery rather than assuming rollback. Low-level users of
`GraphWAL::markApplied` are responsible for the data barrier in step 3.

`GroupCommit` uses a leader/follower condition variable: the leader waits up to
`group_delay` (default 2 ms), syncs all appends accumulated during the window,
and wakes followers whose LSN is covered. `syncCount()` permits testing shared
barriers without relying on timing. This batches concurrent **WAL producers**;
LiveGraph still serializes graph mutations with its graph lock, so this is not
a claim of batched concurrent graph execution or data/checkpoint fsync batching.

`Async` returns after writing log bytes to the OS, with no per-operation durability
barrier. Applied markers remain volatile and are replayed conservatively after
restart. `flush()` syncs log bytes only. For an explicit durable graph checkpoint,
call `LiveGraph::checkpointWAL()` or `GraphManager::checkpointGraph()` (Python:
`checkpoint_graph(name)`). It syncs WAL, graph files and checkpoint in order.
Retention uses the **persisted** checkpoint, including after async checkpoints;
volatile markers never authorize deletion. Closing syncs WAL bytes but does not
promote volatile applied markers. Async operations may be lost after power loss.

The backend uses Linux/POSIX file operations and relies on the filesystem/device
honoring fsync. SIGKILL tests verify process-crash behavior; they are not a physical
power-cut or faulty-storage simulation.

## Rotation, retention and recovery

The default segment limit is 4 MiB. Rotation syncs the old segment, creates the
next segment exclusively and syncs its file and directory before acknowledging
records in it. Retention unlinks only whole sealed segments whose last LSN is at
or below the durable applied checkpoint and whose newest timestamp has expired.
The active segment stays present. Surviving segments are never rewritten.

Startup validates magic, version, size, CRC, operation codes and LSN continuity.
Only a physically incomplete tail in the newest segment is truncated, followed
by fdatasync. A complete record with an invalid CRC, a torn sealed segment,
a missing required prefix, or a corrupt checkpoint causes startup failure with
`lastError()`; those records are never silently discarded.

Redo upserts node/edge IDs and treats repeated removals as successful. Node redo
removal includes incident edge tombstones. Adjacency, degrees, counters and ID
allocation bounds are rebuilt from replayed records; replay persists the result
before checkpointing. A replay failure prevents later records from overtaking it.
Undirected edge direction is retained in flags for reload and repeated replay.

Before compaction remaps physical IDs, LiveGraph requires a durable WAL/data
checkpoint with no pending redo. Compaction publishes the node/edge/identity
generation through the atomic manifest protocol in `GRAPH_STORAGE_V2.md`.

## Configuration and legacy logs

```python
options = nexoradb.WalOptions()
options.durability = nexoradb.WalDurability.Sync
options.segment_bytes = 4 * 1024 * 1024
options.group_delay_ms = 2
manager = nexoradb.GraphManager(db, graph_dir, options)
```

Unknown/raw legacy WALs are rejected by default. An explicit offline migration
option, `migrate_legacy=True`, decodes the historical 60-byte little-endian layout
and atomically replaces it with v2 records in a fresh contiguous LSN epoch.
Retained records preserve operation order and IDs; old sequence gaps are removed.
Old flush-only applied markers are not trusted: every migrated record is replayed.
Create a storage backup before migration. If old logs span a physical-ID-changing
legacy compaction, rebuild the graph from its source instead of replaying stale
IDs. Migration does not retroactively make the old storage format crash-safe.

## Acceptance and reproduction

`tests/cpp/test_graph_wal.cpp` covers immutable encoding and checkpoint prefix,
all 71 possible incomplete tail lengths, record and checkpoint corruption,
segment retention, writer exclusion, three durability policies, concurrent group
sync, abrupt `_exit`, SIGKILL between log/data/checkpoint, repeated node/edge
add/remove redo, async checkpoint/retention, publication failure, missing/torn
sealed segments, and explicit legacy migration.

```bash
cmake --build --preset debug --parallel 2
ctest --test-dir build/debug --output-on-failure
cmake --build build/asan --target nexora_graph_tests --parallel 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 build/asan/tests/cpp/nexora_graph_tests
cmake --build build/ubsan --target nexora_graph_tests --parallel 2
UBSAN_OPTIONS=halt_on_error=1 build/ubsan/tests/cpp/nexora_graph_tests
```

Debug CTest: 64/64 passed. Sanitizer graph suites are run with halt-on-error.
See `docs/benchmarks/graph-wal-phase4.md` for the retained-history scaling check.
