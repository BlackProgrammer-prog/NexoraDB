# Phase 6 scheduler and Python boundary

Implemented 2026-10-20. No persistence format change.

## Bounded scheduler

GraphManager owns a fixed worker pool; production graph code no longer launches
std::async for jobs. Defaults are **2 workers**, **32 queued jobs**, and **512 MiB
of aggregate declared reservations**. Configure SchedulerOptions at construction
(Python: `GraphManager(db, directory, scheduler_options=options)`). Worker count
must be 1..256; queue and reservation limits must be nonzero. Limits are per
manager/job pool, not a limit on RocksDB's independent background threads.

Submission admits work before snapshot construction. The worker builds the
snapshot and runs the algorithm, so rejected/queued work does not retain a graph
snapshot. Its snapshot represents the graph at **worker start**, not submission
time; dropping a graph while a job is queued can make that job fail cleanly.
Snapshot/algorithm execution never holds registry_mutex. Separate graph handles
and the prior registry-progress tests continue to apply.

SnapshotOptions carries job controls:

| Field | Default | Meaning |
|---|---:|---|
| job_priority | 0 | Higher first; FIFO for equal priorities |
| enqueue_timeout_ms | 0 | Reject immediately when full; positive waits at most this admission interval (soft OS scheduling bound) |
| timeout_ms | 0 | Optional total cooperative deadline, including admission, queue, snapshot and execution |
| max_build_bytes | 0 | For managed jobs, zero selects a 64 MiB snapshot allocator cap |
| workspace_reservation_bytes | 64 MiB | Declared workspace admission charge, added to snapshot cap |
| max_result_bytes | 16 MiB | Reject oversized returned JSON before exposing it to Python; zero explicitly disables this check |
| cancellation | new token per job unless supplied | Shared atomic token; C++ JobHandle.cancel or Python options.cancel |

Queue count and aggregate reservation are both checked under one mutex. Overload
returns an unsuccessful result, not a thread launch or unbounded wait. Rejected
jobs never run. Reservation arithmetic is overflow-checked. Reservations cover
running plus queued jobs and are released only after captured data is destroyed.
Nested submission from a worker to its own pool is rejected, avoiding the common
submit-and-wait thread-pool deadlock. Priorities do not preempt running work and
have no starvation/aging guarantee under sustained higher-priority load.

**Budget boundary:** snapshot-owned allocation is enforced by PMR. Workspace
reservation is logical admission, not instrumentation of arbitrary algorithm
allocations. Source graphs, cache templates, argument copies, result construction,
allocator headers and unrelated work are not a process RSS cap. Result-byte
validation occurs after an algorithm constructs its result. Custom algorithms
must respect declared workspace needs; this is not an out-of-memory sandbox.

Timeout/cancellation is cooperative. Built-ins poll snapshot.checkCancelled;
custom JobAlgorithms must do so too. C++ allocation/sort, a blocked native call
or a custom algorithm that never polls cannot be forcibly interrupted. A queued
cancelled/expired promise resolves when a worker dequeues it. No monitor thread
or unsafe thread kill is introduced. Queue time includes admission waiting;
execution time includes snapshot construction and algorithm execution. AlgoResult
reports queue_ms, execution_ms and reserved_bytes separately from the algorithm's
existing elapsed_ms. schedulerStats/scheduler_stats gives worker/queued/running
counts, reservations and stopping state.

The owning submitJob(shared_ptr<JobAlgorithm>, ...) overload pins algorithm
lifetime through completion; every built-in wrapper uses it. The legacy borrowed
reference overload remains source compatible and explicitly requires its caller
to retain the algorithm through result completion. JobHandle destruction no
longer joins a fresh async thread; it is safe to discard an owning job handle.
Result consumption/move remains single-owner; concurrent cancellation is safe
while that handle is stable, but do not race result()/wait/move on one handle.
Do not concurrently reuse one mutable custom algorithm instance unless that
algorithm explicitly supports it; shared ownership is not synchronization of
arbitrary user state.

shutdown is terminal: reject new submissions, signal all running/queued tokens,
resolve queued promises, and join all workers **before** closing graph WAL/data.
It is idempotent. Do not call shutdown/destruct a manager from its own worker.
External callers still must quiesce concurrent non-job API calls before manager
destruction. Shutdown waits for noncooperative jobs; it cannot promise a deadline
for arbitrary user code. Python explicit shutdown releases GIL while joining.

## Streaming output

```python
with db.iter_documents("items", condition, page_size=128,
                       max_page_bytes=4 * 1024 * 1024) as documents:
    for document in documents:  # Python dict, one page retained at a time
        consume(document)

snapshot = manager.create_snapshot("social")
for edge_id, src, dst, type_id, directed in snapshot.iter_edges():
    consume_edge(src, dst)      # no full COO/list/JSON staging
```

DocumentStream pins its engine, copies its predicate and creates a snapshot on
first fetch. Pages have both row and encoded-byte limits; an individually too-
large document raises an error rather than bypassing the byte budget. Byte
limits concern the encoded page, not decoding one document, Python dict overhead
or process RSS. Each page uses the existing full-scan FindPage snapshot contract,
not index-planned streaming, sorted query output or JOIN streaming. Python's JSON
decoder operates per bounded page. The iterator holds no collection-sized JSON.

close/context-manager exit releases a pending page snapshot, and destruction
does so too; engine lifetime is pinned even after deleting its Python variable.
Concurrent/reentrant use of one stream is rejected via try-lock rather than
waiting with GIL held. Existing paging TTL remains five minutes. Keep streams
short-lived because a RocksDB snapshot can retain old storage versions.
SnapshotEdgeStream pins StaticGraph, visits one **logical** edge once, and emits
snapshot-local node IDs; use original_id/ext_id for conversion. It makes no
edge-sized temporary array. Legacy JSON/COO APIs remain for compatibility, not as
the recommended large-output path. Arbitrary algorithm JSON is still a bounded
scalar result; it is not generically converted into a record stream.

## GIL and lifetime audit

The audit covers every gil_scoped_release/call_guard site in
bindings/nexoradb_bind.cpp:

| Sites | Native inputs/lifetime | GIL behavior |
|---|---|---|
| CRUD, bulk, query, JOIN | strings/vectors converted into per-call storage; mutable Condition/UpdateSpec/BulkWriteOptions copied | Native I/O runs released; Python result building is outside release scope |
| schema/index/FK DDL | owned schema/index/FK copies; immutable string arguments | Release while locking/building; avoids blocking a Python transaction owner |
| Graph definition/mappings | GraphDefinition/NodeMappingDef/EdgeMappingDef copied before release | Native persistence runs released |
| graph build/render/refresh and live updates | converted strings and manager kept alive by active Python call | Release; registry owner pins handle internally |
| graph checkpoint/compaction/drop/retention | converted graph name and pinned handle (drop instead rejects active owners) | Release around native persistence |
| traversal | argument copies plus GraphReadLease | Native access released; lease pins backing storage |
| heavy built-ins | params converted per call, SnapshotOptions copied (atomic token intentionally shared), owning algorithm captured | Release while scheduler waits; native pool never calls Python |
| create_snapshot | filters copied by caster, native unique_ptr result | Release only around creation; cast/none after reacquisition |
| explicit transaction CRUD | borrowed TxHandle Python argument retained for entire call; engine pinned by begin_transaction keep_alive | Release; handle remains confined to creating thread, including destruction |
| iterate_collection callback | std::function copy captures owned Python callable | Release around scan, explicitly reacquire around every callback; capture destroyed after outer release guard reacquires GIL |
| DocumentStream page fetch | stream/engine pinned and private copied predicate | Release only for native FindPage; parse/dict creation after reacquisition |
| explicit scheduler shutdown | manager retained by active call | Release while cancelling/joining; no Python worker callbacks |

StaticGraph for_each callbacks and direct small metadata/result conversion APIs
retain GIL; no blanket release guard surrounds Python object creation. Mutable
native configuration objects are not concurrently mutable C++ objects. TxHandle
cannot be shared/reset from another Python thread. Tests cover callback exception
propagation, engine/snapshot deletion before stream exhaustion, early close,
oversize page rejection, GIL/DDL transaction progress and shared native writers.

## Conversion microbenchmark

The standalone nexora_conversion_probe module is test-only and not installed as
part of the database API. It measures boundary dispatch + allocation + pybind11
conversion without DB I/O; it cannot isolate conversion from native allocation.
GCC 11.4 **Debug**, CPython 3.10.12 / WSL; medians of seven passes:

| Payload | Python -> C++ -> Python string | C++ allocate + Python string |
|---|---:|---:|
| 16 B | 0.775 us | 0.605 us |
| 1 KiB | 0.846 us | 0.656 us |
| 1 MiB | 416.713 us | 392.058 us |

| 32-byte strings | Python list -> native vector + sum | native vector -> Python list |
|---|---:|---:|
| 100 | 29.919 us | 14.816 us |
| 10,000 | 2,776.201 us | 1,150.360 us |

This is a baseline, not a claimed optimization or a Release estimate. It supports
bounding list/page sizes and avoiding one massive conversion. Reproduce:

```sh
cmake --build build/debug --target nexora_conversion_probe --parallel 2
PYTHONPATH=build/debug/tests/cpp python3 tests/benchmarks/python_conversion_probe.py
PYTHONPATH=build/debug python3 tests/test_native_threading_contract.py
```

## Fully instrumented TSan

The tsan-full preset uses a separate x64-linux-tsan overlay triplet. RocksDB,
compression libraries, BSON, fmt and test dependencies are compiled with
`-fsanitize=thread -fno-omit-frame-pointer -g`, not linked from the ordinary
x64-linux binaries. RocksDB archive nm output contains __tsan_read* and
__tsan_atomic* references. Engine and dependencies have the same TSan runtime.
The prior Slice/MemTable warnings do not recur in the fully instrumented mixed
DocEngine workload.

On this GCC 11/glibc combination, newer pthread_cond_clockwait is not correctly
tracked by libtsan and produced false double-lock reports in both scheduler and
RocksDB timed transaction waits. cmake/TsanCompat.h selects libstdc++'s portable
timed-wait implementation **in both engine and dependency builds**. It does not
disable detectors, suppress stacks or remove synchronization instrumentation.
Admission/deadline decisions still use steady_clock; wall-clock adjustment can
affect a single timed wait, so timeout remains a cooperative OS-level bound.
The compatibility header applies only to GCC 11 TSan builds.
`nm -D` on this runtime exposes pthread_cond_timedwait and its interceptor but
no pthread_cond_clockwait interceptor, matching the observed diagnostic.

```sh
VCPKG_MAX_CONCURRENCY=2 setarch x86_64 -R cmake --preset tsan-full
cmake --build build/tsan-full --target nexora_graph_tests nexora_doc_tests --parallel 2
ctest --preset tsan-full
TSAN_OPTIONS=halt_on_error=1:history_size=7 setarch x86_64 -R \
  build/tsan-full/tests/cpp/nexora_graph_tests \
  --gtest_filter='ThreadingContract.*:JobScheduler.*:LiveGraphConcurrency.*:GraphSnapshot.ConcurrentLazyCscPublicationAndConsistentSourceEpoch' \
  --gtest_repeat=10 --gtest_brief=1
```

ASLR disabling is the existing WSL startup workaround for TSan's shadow mapping,
not disabling race checks. Selected workload results are recorded after the
final dependency rebuild; they are not a proof about every possible workload.

Final validation: **13/13 selected graph/scheduler/threading tests, repeated ten
times (130 passes)**, plus **36/36 full DocEngine tests**, all with fully
instrumented dependencies and `halt_on_error=1`, no suppressions. The previously
failing concurrent unique-index race, transaction timeout and online-index-build
tests are included in the DocEngine run. Debug CTest is **116/116**; ASan with
leak detection and UBSan each cover **80 graph + 36 document tests**. Python's
subprocess regression passes, including scheduler controls/timing, callbacks,
stream ownership, early close and byte limits. This closes the selected phase-6
acceptance gate, not a claim that every unsupported custom job or arbitrary
interleaving has been proven race-free.
The reproducible `ctest --preset tsan-full` gate also passes **49/49** (the same
36 document/parser tests plus 13 selected graph/threading/scheduler tests).
