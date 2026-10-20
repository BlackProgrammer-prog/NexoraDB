# Production backup, recovery and migration

Scope: POSIX/Linux (including the current WSL target). These commands do not
stop a service, overwrite an existing deployment, or provide an online atomic
transaction spanning Document and Graph. Use the matching native Python module.

## Offline consistent bundle

Stop application writers and close every DocEngine, GraphManager and standalone
GraphStorage/WAL owner first. A bundle captures the **existing quiescent state**
of both engines, including manual graph mutations and unapplied WAL. It does
not retroactively make document events and graph notifications transactional.
No graph rebuild occurs implicitly during restore/recovery.

GraphManager holds a shared `.nexora-admin.lock` lease for its lifetime. Admin
requires its exclusive lease and existing per-graph storage locks, while opening
DocEngine requires the RocksDB LOCK. Lock attempts are nonblocking and fail
closed. The service must still be stopped: older binaries/direct file writers
do not implement the root lease, and directory membership cannot be coordinated
with arbitrary external writers. Shutdown alone does not release a manager's
root lease until its object is destroyed.

```sh
export PYTHONPATH=build/release
python3 scripts/nexora_admin.py backup --db /data/documents --graphs /data/graphs --output /backups/day1
python3 scripts/nexora_admin.py restore --bundle /backups/day1 --output /recovery/day1
```

The bundle contains `documents/`, `graphs/`, and a versioned `bundle.json`.
Documents use RocksDB Checkpoint (forced flush), not a live directory copy.
Its returned sequence is an **included lower bound**, not an asserted exact
latest checkpoint timestamp. Graph files and definitions are copied while
quiescent; copied WAL is replayed and storage validated in the staging tree.
The source graph files are not normalized or rebuilt. A `recover_stored_graphs`
maintenance API refuses missing manifests/bad definitions and never invokes
source-based auto-build. Building/corrupt graph states and orphan graph
directories fail closed rather than being exposed as a complete recovered graph.
Admin pauses and joins the automatic document migration worker before capture
or validation, so it cannot change legacy-document counts between validation
and file hashing. This pause API is offline maintenance: do not call it while
holding a transaction or concurrently destroying the engine.
Checkpoint can hard-link immutable SSTs on the same filesystem. Treat a
published bundle as immutable; place independent/off-site copies on a separate
failure domain. A local checkpoint is not protection against losing that disk.

Every staged regular file has SHA-256 and length recorded; symlinks and special
files are rejected. SHA-256 detects corruption, not malicious replacement of
both files and manifest: authenticate/encrypt backups separately as required.
File contents and directories are fsynced. Linux `renameat2(RENAME_NOREPLACE)`
publishes the complete sibling staging directory atomically, followed by parent
directory fsync. Existing outputs, including a concurrent creation race, are
never overwritten. A post-rename sync failure is reported as an ambiguous result;
the complete target may already exist. Do not blindly rerun into that target.

Restore verifies the complete file set before and after copying, then opens
the restored copy, checks SST checksums/document envelopes/secondary indexes,
loads graph storage and replays WAL. Counts and format reports must match the
backup, including a streamed SHA-256 fingerprint of node identities/types and
outgoing edge traversal. Recovery-normalized files receive a refreshed manifest before publication.
Writable restored SSTs are copied, never linked back into the immutable backup.
The backup must not be modified concurrently with restore. Interrupted/failed
operations retain named `.staging-*` directories for diagnosis; incomplete
staging trees are not published or valid backups. There is no automatic cleanup
of user data and no automatic service cutover.

## Integrity and rebuild

```sh
python3 scripts/nexora_admin.py integrity --db /data/documents
python3 scripts/nexora_admin.py rebuild-indexes --db /data/documents --collection users
python3 scripts/nexora_admin.py rebuild-graphs --db /data/documents --graphs /data/graphs --output /recovery/rebuilt
```

Integrity checks verify RocksDB SST checksums, decode every document, validate
index metadata/Ready state, check every expected E/U entry and ownership, every
actual entry against its source document, and orphan idx2 keyspaces. One read
epoch spans each logical check; scans avoid cache pollution. Metadata-sized
state is retained, not a full expected-entry set. This is not a schema/FK,
collection-counter or application-invariant verifier; a checksum-clean stale
source document can still violate an application invariant.

Index rebuild uses the existing atomic batch path; it can require substantial
memory for very large indexes and is offline maintenance, not an unbounded
production background task. Graph rebuild creates a **new output bundle** from
a document checkpoint and graph definitions. It does not clear the original
graph or overwrite it on failure. Source reconstruction intentionally discards
graph-only mutations in the new output; use backup/restore when they matter.
Current graph-definition v2 is required for rebuild; unsupported legacy graph
definitions need an explicit converter, not silent omission.

## Independent versions and reader fence

The component manifest `meta:format:components` independently records:

| Component | Supported version |
|---|---:|
| Component manifest schema | 1 |
| Minimum reader generation | 2 |
| Document key encoding | 1 |
| Physical document envelope | 1 (NXD1/BSON) |
| Secondary indexes | 2 (idx2/NXI2) |
| Graph storage | 2 |
| Graph WAL | 2 |

Manifest values are exact supported requirements, not a claim that an external
graph directory has been inventoried by DocEngine. Bundle validation checks
the actual graph files. Unknown/missing components and future versions fail
startup before internal collection initialization. Existing unmanifested stores
remain accepted for explicit upgrade; opening them does not silently install a
new reader fence.

```sh
python3 scripts/nexora_admin.py migrate --db /data/documents --dry-run
# Keep a pre-upgrade bundle before the following command:
python3 scripts/nexora_admin.py migrate --db /data/documents
```

Dry-run uses a quiet read-only RocksDB open under an offline fcntl LOCK lease;
it neither creates internal collections nor starts migration workers. It reports
document bytes/count, legacy documents/indexes and supported versions. Actual
migration emits JSON progress per resumable document chunk, rebuilds indexes,
performs integrity verification, then atomically and synchronously installs the
component manifest and reader fence. Document chunks resume via the existing
persistent cursor; index rebuild is atomic per collection. No progress callback
holds the Python GIL while native work executes.

The existing `meta:format:documents` marker becomes **2 (store reader generation)**
only on explicit upgrade; physical envelopes remain version 1. Pre-phase-8
binaries that require marker 1 reject this marker before initializing the store.
Current migration recognizes both completed markers, so a background worker
cannot erase the fence. This does not retroactively protect against arbitrary
ancient/custom binaries that never checked this marker.

Rollback is restore of the pre-upgrade bundle to a new directory with the old
binary's supported formats, not editing version numbers backward in place.
Writes made after the backup are not part of that rollback; reconcile them
explicitly or accept the documented recovery point. Native tests exercise the
pre-phase-8 marker rule, upgraded reopen, unknown-version rejection, and opening
the pre-upgrade checkpoint. In addition the integration suite was run with an
actual reader linked against archived phase-7 UBSan core/query/RocksDB libraries:
it opened the pre-upgrade checkpoint, rejected the upgraded store with
`Unsupported document format version: 2`, then opened the rollback checkpoint.
No every-historical-release or in-place reverse-converter claim is made.

That archived core's SHA-256 was
`66abbc28b78fa232b5afa11a234dccda70ece3ae0e54e17eae8ba48f3693f6bb`;
the linked `/tmp/nexora_phase7_reader` probe was
`4989ddcf44f086ff19b6def01daef0f1519dc084f347822c723e7b396d91c515`.
The probe source is `tests/cpp/compat_reader_probe.cpp`, intentionally not linked
to current nexora_core by CMake. Supply an ABI-compatible archived reader via
`LEGACY_READER_PROBE=/path/to/probe`; otherwise that integration test skips.
For a release pipeline retain/build the prior tagged release's reader rather
than relying on a temporary workspace archive.

## Validation and recovery timing

`tests/test_admin_recovery.py` covers cross-process ownership rejection, dry-run
byte stability, backup/restore parity, fresh-output enforcement, source graph
rebuild parity, invalid graph format, and corrupt backup rejection.
`Maintenance` native tests cover checkpoint isolation and no overwrite, stale
index detection/rebuild, invalid documents, upgrade fence and rollback checkpoint.
`GraphRecovery` verifies fail-closed handling of an interrupted build and orphan
directory. Final runs: Debug **126/126**, ASan **126/126**, UBSan **126/126**,
and selected fully instrumented TSan (including RocksDB dependencies)
**59/59**, without suppressions. Two Python recovery/archived-reader integration
tests passed, plus the existing native threading/GIL regression. These are
software failure/ownership tests, not physical power-cut validation.

```sh
PYTHONPATH=build/debug python3 tests/test_admin_recovery.py
ctest --test-dir build/debug -R Maintenance --output-on-failure
PYTHONPATH=build/release python3 tests/benchmarks/recovery_probe.py 100000
```

The non-CI timing fixture uses 100,000 nodes, 100,000 directed ring edges,
200,000 source documents, 1 KiB independently seeded random user payloads, and
a secondary index. Three restores time checksum verification, copy, native
integrity, graph load/replay, fsync and publication to readiness, not just the
directory copy. An engineering acceptance target of 60 seconds per restore is
used for this local fixture; it is not a negotiated business RTO or large-scale
production SLA. WSL page cache is warm; no physical power-loss claim is made.

Measured 2026-10-20, GCC 11.4 Release/LTO, RocksDB 8.5.3 on WSL2/ext4,
i9-14900K (32 logical CPUs visible):

- Bundle: 149,560,665 logical file bytes (not unique allocated disk blocks).
- Fixture load/build: 7.620 s; backup/validation/publication: 7.532 s.
- Three full verified restores: **7.535 s, 7.402 s, 7.405 s**; all below 60 s.
- Each restored graph matched 100,000 nodes, 100,000 edges, and traversal hash
  `cc4d4fe331b3943697349395490be1de34c96c183f09123895bb3fbe1263f21d`.

Raw measurement: [recovery_phase8_wsl.json](../../tests/benchmarks/recovery_phase8_wsl.json).
No other build/test workload ran during the measured recovery fixture. Restore
includes the CSR snapshot used for fingerprint verification; much larger graphs
can exceed the existing snapshot build budget and fail closed. Re-evaluate
capacity/budgets and RTO with deployment-scale data instead of extrapolating
these warm-cache, approximately 143 MiB results to terabytes.
