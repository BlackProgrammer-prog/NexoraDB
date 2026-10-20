# Graph storage v2 and atomic compaction

Implemented 2026-10-19. The Graph Storage Format and correct-compaction
checklists in the C++ roadmap are implemented. Source-document/graph event
reconciliation remains separate work.

## File set

`<graph>.manifest` selects one generation. Generation files are named
`<graph>.g<20-digit-generation>.nex`, `.nexr`, and `.nexl` for nodes, edges,
and metadata. Opening an existing graph requires all three referenced files.
There is no fallback to an older generation when the selected one is corrupt:
that could silently discard acknowledged writes. A missing manifest beside
existing generations is an error, never an instruction to create an empty graph.
An exclusive POSIX lock on `<graph>.storage.lock` prevents competing owners.

All integers introduced by the physical format are explicitly little endian.
Native `sizeof`, enum layout, bool layout, padding, and packing are not used to
serialize any GraphStorage file. `StorageCodec.h` is the format boundary.

Each generation file begins with this 32-byte header:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | `NXG2` magic |
| 4 | 4 | Version, 2 |
| 8 | 4 | Kind: nodes=1, edges=2, metadata=3 |
| 12 | 4 | Record size: 40 for nodes/edges, 0 for bounded variable metadata |
| 16 | 4 | Checksum policy: 1 = mandatory IEEE CRC32 |
| 20 | 8 | Generation |
| 28 | 4 | CRC32 of the header with this field zeroed |

Node records contain dense ID (8), type ID (4), flags (4), out-degree (8),
in-degree (8), CRC32 (4), and reserved zero (4). Edge records contain edge ID
(8), source (8), destination (8), type ID (4), flags (4), CRC32 (4), reserved
zero (4). Each record CRC covers the complete 40 bytes with its own slot zeroed.
Record offset is `32 + ID * 40`. Sparse slots use explicitly encoded empty
records with valid checksums and the correct slot ID.

Metadata uses its own v2 header followed by JSON byte length (8), UTF-8 JSON,
and CRC32 (4) over length plus JSON. The JSON size limit is 16 MiB. It contains
named logical GraphMeta fields and `identities`, `node_types`, `edge_types`
dictionaries. GraphMeta's logical version is distinct from physical format v2.
Metadata is published using the same atomic-file/fsync helper as the manifest.

The 32-byte manifest contains `NXM2` (4), version 2 (4), generation (8), reserved
zero (12), and CRC32 (4), with its own slot zeroed when computing the CRC.

## Identity and startup validation

External node IDs and type dictionaries are persisted before their node/edge
redo records can be acknowledged. Intent identities whose node slot is inactive
are ignored during load. This also supports WAL redo after a crash between
identity persistence and node-record persistence.

LiveGraph restores both directions of ExtId/DenseId and restores type-name/ID
registries from the selected generation. Compaction remaps identity dictionary
keys together with nodes and edge endpoints. Undirected flags survive reload;
adjacency and degree updates retain both directions. Implicit-to-explicit node
promotion persists the explicit type.

Startup validates every header, checksum policy, generation, file length, record
CRC, slot ID, flags, active edge endpoints, and metadata/type dictionary shape.
Duplicate active external IDs are rejected. Unknown versions, missing files,
truncation and corruption fail `open()` with `lastError()`. Runtime read/scan
corruption throws an error, poisons subsequent storage writes, and includes
file/slot context. Scans never report partial input as a successful full scan.
Logical metadata counters are cached values; the actual records determine
LiveGraph counters and the compacted snapshot's counts.

## Compaction commit protocol

The graph write lock and storage mutex exclude mutations during compaction.

1. Require no pending redo; durably checkpoint WAL and graph data, including
   volatile Async applied markers. This fences redo containing old physical IDs.
2. Validate the selected generation and collect active nodes and edges. Build
   node and edge remaps. Remap every edge source/destination; an invalid active
   endpoint is corruption and causes failure rather than silent edge removal.
3. Build new node/edge records and remap the identity dictionary. Recompute
   degrees and metadata counts. Select a fresh unused generation number.
4. Write the three new-generation files through exclusive temporary files.
   Each temporary file is fsynced, renamed into its unreferenced generation
   filename, then its directory is fsynced. No selected file is truncated.
5. Publish the new manifest using exclusive temporary write, fsync, atomic rename
   and directory fsync. This is the file-set commit point.
6. Activate and validate the selected generation, then update LiveGraph RAM.

The previous generation remains intact through commit and is retained afterward.
Unreferenced staging generations and temporary files are ignored at startup.
Retry uses a fresh generation number, so orphan staging does not overwrite an
earlier file set. Automatic old-generation garbage collection is not implemented;
retention trades disk space for recoverability and must be handled separately.

A failure after manifest rename can have an ambiguous commit outcome. The API
reports failure and requires close/reopen; startup follows the manifest and
selects a complete file set. Never continue mutating the pre-failure LiveGraph
instance. This is a POSIX/Linux contract relying on filesystem fsync semantics.
Process-kill tests are not physical power-cut or failing-device simulations.

The compatibility `compactNodes`/`compactEdges` pair is retained: the first only
plans a node remap; the second verifies it and atomically commits both files and
metadata. New callers should use the combined `compact` method.

## Legacy data and operational limits

Raw legacy `.nex/.nexr/graph_meta.nexl` files are rejected explicitly, preserved,
and never guessed as v2 data. Before upgrading an existing graph directory:

1. Stop graph writers and back up the graph directory and authoritative DocEngine.
2. Build into a new empty graph directory using the existing source collections.
   Recreate graph definitions/mappings via the supported API or carry over only
   validated version-2 `.graphdef` definition files.
3. Validate node/edge counts, external-ID lookups and representative traversal.
4. Switch the configured graph directory to the rebuilt generation store; keep
   the old directory as the rollback copy.

This is a rebuild migration plan, not an in-place converter for compiler-dependent
legacy bytes. No user database is migrated by the test suite or by opening v2.
Checkpointing and compaction stage an in-memory snapshot; peak memory scales
with graph size. Identity metadata currently rewrites a bounded JSON dictionary;
batching and a larger-scale identity storage scheme remain ingestion optimization
work, not an unmeasured performance improvement claimed by this change.

## Verification

`tests/cpp/test_graph_storage.cpp` checks fixed byte encoding, numeric boundaries,
header/record/manifest/metadata corruption, truncated/missing files, runtime scan
errors, structural corruption with recomputed valid CRC, writer exclusion, legacy
rejection, Unicode/escaped IDs, promotion, remapped identity/type reload, and
undirected traversal. It also tests Async WAL fencing before compaction and the
compatibility two-call API.

SIGKILL is injected at six boundaries: after staged nodes, edges, metadata,
before manifest publication, after durable manifest publication, and after file
activation. Each restart must select exactly the complete old or new generation,
retain correct ExtIds and traversal, and allow another successful compaction.
Before the manifest switch, a crash within temporary-file writing leaves the
previous manifest selected. Atomic rename provides the old/new choice during
manifest publication; both referenced file sets have already been synced.

Reproduce with the Debug CTest suite and the graph suites in ASan/UBSan:

Acceptance run: 77/77 Debug CTest cases passed; 41/41 graph cases passed in
each of ASan (leak detection and halt-on-error) and UBSan (halt-on-error).

```bash
cmake --build --preset debug --parallel 2
ctest --test-dir build/debug --output-on-failure
cmake --build build/asan --target nexora_graph_tests --parallel 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 build/asan/tests/cpp/nexora_graph_tests
cmake --build build/ubsan --target nexora_graph_tests --parallel 2
UBSAN_OPTIONS=halt_on_error=1 build/ubsan/tests/cpp/nexora_graph_tests
```
