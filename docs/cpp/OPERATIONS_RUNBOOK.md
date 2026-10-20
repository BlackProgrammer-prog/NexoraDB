# Operations, failure response and performance gates

Scope: Linux/WSL, matching native extension. This complements
[backup/recovery](PRODUCTION_RECOVERY.md) and [RocksDB settings](ROCKSDB_TUNING.md).
No remote Actions execution or fixed-runner registration is implied by a local
test pass. Repository workflows must be committed/enabled to run remotely.

## Live metrics and slow queries

The application's existing engine owner can call `operation_metrics()` and
`health_report()` (JSON), or embed `scripts/operations_metrics.py`'s `collect()`
and `prometheus()` in its authenticated metrics endpoint. Do not open a second
DocEngine against a live path just to scrape metrics: RocksDB LOCK forbids it.
No HTTP port, unauthenticated endpoint, credentials, or service is created here.

Native fixed labels are query execution, point read, insert, batch insert,
update-by-ID and delete-by-ID. These are API-operation metrics, not complete
HTTP request coverage: metadata, paging, explicit Tx variants, joins' outer
execution and graph algorithms are not independently counted. An internal query
used by Explain/join is a query operation, not an extra client request. Batch
insert is one call, not one observation per document. Instrument service ingress
separately for an end-to-end SLO or all-API throughput.

- Counters: calls, errors and not-found. Not-found contributes to calls and its
  own counter, not errors. Exceptions and validation failures count as errors.
- Duration histogram: cumulative microsecond power-of-two boundaries converted
  to seconds, sum/count, with a final +Inf bucket. Max and throughput/error-rate
  JSON values are lifetime values, not rolling-window percentiles.
- Standard monitoring uses `rate(nexora_operation_duration_seconds_count[5m])`,
  `rate(nexora_operation_errors_total[5m])`, and
  `histogram_quantile(0.99, rate(nexora_operation_duration_seconds_bucket[5m]))`.
- RocksDB cache, Bloom, compaction debt, running work, stalls and byte counters
  are included. Queue metrics expose workers, queued/running jobs, reservations
  and stopping state; per-graph WAL exposes total/pending records. Job results
  retain their existing queue_ms/execution_ms fields. Metrics are sampled,
  not a transactional cross-engine view. No filesystem traversal is added.

Call `configure_slow_queries(threshold_us, capacity=128)`; threshold 0 disables
capture, maximum capacity 4096. The default is 100 ms and 128 events. Query
execution records its **actual** selected scan/index, index/documents scanned,
matched count, success, duration, sequence and process-relative timestamp. No
second query is run to obtain ExplainPlan. Query text, filter literals and
document values are not retained; the index name is capped at 128 characters.
`drain_slow_queries()` atomically drains JSON events for an application log sink
(add a UTC receipt timestamp/trace context there). On overflow the oldest event
is dropped and `slow_events_dropped` increments; this is bounded diagnostic
logging, not a durable audit log. Do not use slow logs as proof of every request.

Reference conventions: [Prometheus histograms](https://prometheus.io/docs/practices/histograms/).

## Health and graceful shutdown

`is_healthy()` now uses `health_report()` readiness, not merely existence of a
RocksDB property. Health checks require background-error/write-stopped/live
version properties and report accumulated background errors, stopped writes,
observed corruption, observed I/O failure and quiescing state. Corruption/I/O
latches are conservative until engine restart; integrity-check failures latch
corruption. Unknown latent disk corruption requires an explicit integrity scan;
a green health sample is not a replacement for it. Health does not issue disk
writes as a probe. File counts and cache hit rates alone are not health signals.

Shutdown protocol (the caller owns admission and transaction lifetimes):

1. Stop accepting new requests; drain/rollback active transactions and finish
   external mutations. Never call prepare_shutdown while holding a transaction.
2. `manager.prepare_shutdown()` joins the bounded scheduler, then checkpoints
   each graph's WAL/data under its operation lock. Check its return value.
3. `manager.shutdown()` closes graph handles. Destroy manager after releasing
   all external leases. It must not race external direct LiveGraph users.
4. `engine.prepare_shutdown()` marks readiness false, stops/joins migration,
   waits for flush, then syncs WAL. Check the returned DBResult.
5. Destroy engine after streams, callbacks and other native users are gone.

Preparation is not an object-lifetime close: methods do not dereference a deleted
DB, but the application must not resume admission afterward. The API does not
silently revoke live transactions. Background/I/O/corruption errors refuse a
clean shutdown result. A destructor is not evidence of successful durability.
In RocksDB 8.5 debug builds, retrying SyncWAL after a failed WAL append can assert;
the destructor now avoids this retry when the error is observed/reported rather
than advertising a successful final sync. Buffered writes before an I/O failure
have no new durability guarantee. Use recovery, do not clear a latch and continue.

## Incident runbook

| Symptom | Safe response |
|---|---|
| Disk full / write error | Stop writes/admission, preserve DB/WAL, inspect free bytes **and inodes**, free unrelated recoverable data or expand storage. Never delete live WAL/SST to create room. Treat shutdown failure as unclean; reopen offline, run integrity, verify recovery before traffic. |
| Permission denied | Stop writes; verify service UID, parent-directory traversal, ownership/mode and mount read-only state. Correct only the explicitly identified paths. Do not run a privileged DB process or `chmod -R 777` as a workaround. |
| Background errors / write stalls | Inspect background_errors, pending compaction bytes, L0, delayed/stopped state and stall deltas alongside latency. Reduce ingestion or increase capacity; inspect actual I/O errors before tuning jobs/buffers. The WSL matrix found low rate limits/small buffers counterproductive. |
| Corruption / unknown format | Quarantine an immutable copy, retain diagnostics and binary version. Do not edit version markers or suppress checksums. Validate a known-good bundle; restore to a fresh path, verify traversal/integrity, then deliberate cutover. |
| Lock contention at startup | Find the existing owner; stop it gracefully. Never delete LOCK files to bypass a live process. Graph root leases intentionally survive shutdown until object destruction. |
| Incomplete backup/build | Do not publish staging as valid. Preserve `.staging-*` diagnostics; restore a complete bundle or explicitly rebuild a new graph bundle from source collections. Preserve graph-only mutations with backup/restore. |
| Regression gate failed | Review raw repetitions, errors, host/build compatibility, noise and latency tails. Reproduce on the same idle host. Rebaseline only through explicit trusted approval with an explanation; never auto-approve on failure. |

Fault tests inject ENOSPC/EACCES at RocksDB WAL file append, assert failed atomic
insertion/readiness and safe destruction. Python additionally tests actual POSIX
permission denial on checkpoint output under an unprivileged UID. These are not
a physical-full-volume, kernel I/O error, or power-cut certification.

## CI, scheduled recovery and history

`.github/workflows/native-quality.yml` defines PR/push/manual/nightly unit jobs
with separate Debug, ASan, UBSan and fully dependency-instrumented TSan builds.
Debug also runs native Python threading, backup/restore, operations metrics and
benchmark-gate tests. Native tests include process-exit/crash WAL/compaction
recovery. vcpkg is pinned to the manifest baseline. Sanitizer logs are artifacts;
no race suppression is introduced.
TSan's test process uses `setarch -R` for GCC runtime/address-layout compatibility;
it does not change host-wide ASLR or suppress race reports. A runner that forbids
this personality change must be configured appropriately; the job fails rather
than silently skipping tests. The archived-reader test requires a previous
release binary and skips without LEGACY_READER_PROBE; do not interpret that skip
as a cross-release compatibility pass.

`.github/workflows/performance.yml` is separate and never runs PR code on the
persistent runner. It requires a trusted idle Linux runner labelled `nexora-perf`
and repository variable `NEXORA_PERF_RUNNER_ENABLED=true`. Until registration and
enablement, it intentionally skips instead of claiming a fixed-host benchmark.
Use one fixed CPU/compiler/power profile; install build/Python prerequisites and
do not run unrelated workloads concurrently. See [GitHub runner labels](https://docs.github.com/en/actions/how-tos/manage-runners/self-hosted-runners/apply-labels).

### Selected host and activation handoff

The user confirmed the existing Ubuntu WSL machine as the fixed host on
2026-10-21. Local inspection found neither GitHub CLI (`gh`) nor an existing
`/home/parham/actions-runner` installation; no registration or persistent service
was performed. Registration requires repository administrator access.

1. Commit/push the reviewed workflow/scripts first; no commit/push is automatic.
2. Open repository Settings → Actions → Runners → New self-hosted runner;
   choose Linux x64 and follow GitHub's current download/checksum instructions.
   Install outside this shared checkout, e.g. `/home/parham/actions-runner`, not
   `/home/parham/NexoraDB` (runner checkout cleaning must not touch the workspace).
3. Configure the repository URL `https://github.com/BlackProgrammer-prog/NexoraDB`
   with name `nexora-wsl-fixed` and additional label `nexora-perf`. Enter the
   registration token locally only; never send credentials in chat or commit them.
4. Start the runner using GitHub's documented foreground/service instructions.
   Verify it appears Idle in GitHub. Windows sleep/WSL shutdown interrupts this
   host; periodic runs require it to remain available and idle.
5. Set Actions repository variable `NEXORA_PERF_RUNNER_ENABLED` to `true`.
6. Dispatch Fixed-host performance history on the trusted default branch once
   with `accept_baseline=true` and `full=true`, after reviewing the clean build.
   Then dispatch with approval disabled and inspect the gate/history artifacts.
7. Verify an actual scheduled native recovery run and fixed-host benchmark run
   before ticking the remaining phase-8 periodic-execution exit criteria.

Official setup: [adding self-hosted runners](https://docs.github.com/en/actions/how-tos/manage-runners/self-hosted-runners/add-runners).

Daily short matrix: baseline A/B/C, 5,000 records, 20,000 operations, five repeats.
Sunday/full manual: full profile matrix, 50,000 records/operations, three repeats,
plus 100,000-node/edge recovery fixture (local engineering RTO bound 60 s).
Crons are UTC. The native quality schedule also repeats the crash/recovery tests.

`scripts/benchmark_history.py` requires matching host fingerprint, CPU affinity,
kernel, operation dimensions, compiler flags/build profile and identical groups.
It fails missing baseline, incomplete samples, incompatible environment and any
operation error. At least three repetitions are mandatory. Meaningful regressions:

- Throughput median down more than 15%, at least 100 ops/s absolute, with all new
  samples below all baseline samples.
- Read/write P99 median up more than 25%, at least 10 µs absolute, with all new
  samples above all baseline samples.

This conservative non-overlap rule filters noise; it can miss regressions whose
ranges overlap and is not a statistical significance test. Coverage is the chosen
DocEngine/recovery workloads, not every algorithm. No universal latency SLA is
implied. Baseline creation/replacement requires manual `--accept-baseline` (or
the trusted workflow input); first scheduled runs fail without initialization.

History JSONL and baseline live outside the checkout in the dedicated runner's
`RUNNER_WORKSPACE/nexora-perf-history`. Raw samples, history and generated HTML/SVG
charts are uploaded (90-day artifact retention). Persistent history itself needs
normal host backup/retention. Charts filter incompatible host/config/builds.
Revision and dirty-worktree state are recorded; local uncommitted results are
not mislabeled as a release benchmark. Failed gates are retained in history too.

Local verification on 2026-10-21: Debug 128/128, ASan 128/128, UBSan 128/128,
and selected fully dependency-instrumented TSan 61/61 passed without suppressions.
Python: two recovery/archived-reader cases, one threading case, two operations
cases (including real unprivileged permission denial), and three gate cases
passed. Workflow YAML parsed locally; no remote Actions run is claimed.
Two independent local short benchmark executions also passed the same-host gate;
[example chart](../benchmarks/operations_history_example.html) and
[history JSONL](../../tests/benchmarks/operations_history_example.jsonl) are
retained as an end-to-end tool demonstration, not a long-term performance trend.
