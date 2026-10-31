# Memory and shutdown changes

## Current acceptance — 2026-10-31

The fresh user-run `mem007_user_20261031` passed limited **10M document** memory
acceptance: two independent loads, all 24 workload cells and all full integrity
checks, zero high/max/OOM/swap/full-PSI deltas, and close in 1.06/1.55 seconds.
See the [audited scope, allocator retention and cleanup intervention](../../docs/cpp/MEMORY_ACCEPTANCE_10M_FA.md)
and [separate, unexecuted 100M plan](../../docs/cpp/MEMORY_100M_PLAN_FA.md).
RES-004 remains an independent open release prerequisite.

The October 23 trim policy described below is historical. Current samplers and
protocol stages observe allocator state without explicit malloc_trim calls;
the current run records startup glibc arena/trim environment settings separately.
Neither the historical completed 100M run nor this accepted 10M run certifies
current 100M capacity or the final 0.5.0 package.

The historical 100M run activated memory.high in all twelve workload cells.
Its close stalled under reclaim pressure; temporary 6-to-8GiB soft-limit relief
allowed completion. This is evidence of pressure, not proof of a single leak.

## Changes and observability

- `DocEngine.memory_report()` reports cache usage/pins, memtable usage/budget,
  table-reader estimates and retained memtables, snapshot count/age and native
  page sessions. These counters overlap: index/filter blocks are in cache;
  never sum them as independent allocations. Unknown properties are null.
- Resource samples include glibc mallinfo2 allocated/free arena and mmap sizes,
  Python allocated-block count and optional existing tracemalloc readings.
  Tracing is not silently enabled during timed workloads. Allocator estimates
  are process-wide, not exact live Python/native-object attribution. Free arenas
  can be fragmented. Custom allocators may not be represented by glibc counters.
- cgroup samples include charged swap and pressure; compare event deltas, not
  historical cumulative totals. `stage-memory.jsonl` records native/allocator/
  cgroup snapshots around stages; load metrics also include native memory.
- Native pagination rejects >1000 rows or >64MiB requested byte budget. The
  default page budget is 4MiB. A document larger than that needs an explicit
  supported larger budget or fails instead of allocating an unbounded page.
- Snapshot idle TTL is two minutes, renewed on accepted continuations, with a
  background reaper even when no new pages are requested. Registry limit stays
  128. In-flight pages retain shared snapshot ownership; expiration never frees
  a snapshot underneath a running reader. Subsequent expired tokens fail. Active
  scans keep the same snapshot beyond five minutes; the former absolute cap
  caused a 10M full-validation scan to fail and has been removed.
- Shutdown stops/joins maintenance workers, releases registry snapshots and
  unreferenced cache entries before flush/WAL sync. Stop accepting requests and
  drain transactions/readers before calling prepare_shutdown. No forced data
  discard, disabled WAL or skipped flush was introduced.

## Budget and cleanup policy

Contained preflight now requires at least 2GiB between cache/memtable budget and
memory.high (6GiB). This is a conservative admission guard, NOT a process-wide
hard allocation limit or proof against OOM. The capacity profile now uses 3GiB
native budget / 2GiB cache, leaving 3GiB below high for overhead and charged cache.
The historical matched-cache profile remains unchanged and is now rejected by
the headroom guard: do not silently reinterpret it as matched under new settings.
Old result snapshots/manifests remain unchanged and must not be resumed.

After timed measurements/restoration end, cleanup temporarily uses the existing
8GiB hard cap as its soft threshold, records `cleanup-memory.json`, and restores
the previous soft threshold in finally. Hard RAM/swap limits never increase.
This is explicit cleanup-only pressure relief, not a performance measurement.
SIGKILL/host termination can bypass finally; normal group preparation resets
limits. Shutdown still cannot promise a hard wall-clock bound under disk failure
or exhausted hard-cap memory; do not force-kill a flushing engine automatically.

## Verification and remaining acceptance

MEM-001/002 supersede the one-shot probe below with a versioned ownership schema
and repeated, paired lifecycle workers: see `docs/cpp/MEMORY_OWNERSHIP_FA.md` and
`docs/cpp/MEMORY_RETENTION_FA.md` at repository root. Resource sampling and protocol
stages now observe allocator state without calling `malloc_trim`; trim is an
explicit diagnostic arm only. Historical manifests/results are not rewritten.

Run `scripts/benchmark_memory_lifecycle.py` with the lab venv from repository root
for a bounded 50k lifecycle probe: open, load, retain 32 snapshots, prepare close,
destroy and optionally trim glibc after destruction. It prints telemetry and
removes only its own temporary store. Trimming in that probe is diagnostic.

## Allocator retention failure, 2026-10-23

Run `memory10m_20261022_171641` completed repetition one but failed in the final
restore of repetition two with `CommitTransaction: Operation expired`. The last
full validation took about 70 minutes, versus roughly four minutes previously.
Stage telemetry showed ~7.4GB glibc arena space, ~2.45GB allocated arena bytes,
~4.96GB free arena bytes, full 1GiB worker swap and rapidly increasing high events.
No cgroup OOM was recorded. This is strong evidence of allocator retention plus
reclaim pressure contributing to the expired restoration transaction, rather than
the earlier cursor expiry. Free arena bytes are not all physically resident or
reclaimable; they must not be treated as a leak size or guaranteed freed RAM.

The memory-test launcher now sets MALLOC_ARENA_MAX=2 and
MALLOC_TRIM_THRESHOLD_=131072 before Python starts. Native deployment sampling
also performs explicit malloc_trim maintenance when free glibc arena bytes exceed
256MiB and at least 25% of arena space, at most once every five seconds; stage
transitions may request an additional trim above the same thresholds. Maintenance
is serialized and logged, including duration, in resource/stage telemetry.
Its CPU/latency cost is part of the run. Live objects, RocksDB caches, transaction
limits, WAL and memory hard limits are unchanged. Unsupported allocators skip it.
This is benchmark-process memory management; it is not yet a server-wide native
allocator policy. New measurements must record the changed allocator policy and
must not be pooled blindly with old results.

A bounded eight-thread retention probe allocated 512MiB temporarily, retained
4MiB of live buffers, then reclaimed unused pages: RSS decreased from 523.44MiB
to 22.84MiB in ~11ms; live buffers remained readable. Six focused maintenance
tests and the complete 53-test lab suite passed. This does not certify 10M memory
stability; the failed run remains failed and its artifacts are preserved.

Small regressions cover page bounds, snapshot release, allocator diagnostics,
and cleanup threshold restoration on failure. The bounded probe cannot certify
100M memory stability. Remaining acceptance requires a long, repeated dataset
run with stable RSS/PSS/anon, no new high/max/OOM events, and observed shutdown
time. Graph memory, allocator fragmentation and general buffered query results
are not completely controlled by the RocksDB budget and remain investigation
targets. Do not claim this phase fully accepted before that evidence exists.

Local verification of this revision: 58 C++ tests, 88 API/Python tests, 49 lab
tests and one native threading regression passed (196 total). The bounded 50k
probe retained 32 sessions and released all at prepare_shutdown (~0.164s).
RSS was 28.75MiB open, 63.73MiB loaded, 65.23MiB after preparation,
33.61MiB after engine destruction, and 30.85MiB after diagnostic allocator trim.
This demonstrates why an RSS reading immediately after flush is not proof of a
leak; engine destruction and allocator retention must be distinguished. It does
not establish the absence of leaks or throttling in a 100M experiment.
