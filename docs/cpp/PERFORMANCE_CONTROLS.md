# Performance pass and operator controls — 2026-10-22 (Asia/Tehran)

This is a measured optimization pass, not a claim that Nexora is now faster than
MongoDB or can ingest millions of arbitrary JSON documents per second. No 100M
experiment was started in this pass. Existing observations remain archived with
their original binary/config provenance; they are not new-version measurements.

## Findings

1. NexoraQL previously cached only the Lark parser instance, NOT repeated query
   syntax. Earley parsing ran on every request. The new cache stores syntax trees
   before parameter binding, never query results, authorization decisions or bound
   parameter values. Each hit receives an independent tree/AST and parameters are
   validated/bound again. Exact query text is the key; there is no unsafe literal
   normalization. Errors are not negatively cached. Limits are 256 entries, 8MiB
   conservatively accounted Python object bytes, 300s TTL, 16KiB query eligibility
   and a per-entry size guard. These accounting limits are not a process RSS cap.
   Cache construction, eviction and cold-key publication are synchronized.
2. Update/restore rewrote every secondary-index entry even when its encoded tuple
   did not change. READY index entries now remain untouched in that case. Changed
   keys still maintain indexes and enforce uniqueness. BUILDING indexes retain
   the old unconditional maintenance path to preserve online-build semantics.
3. Native storage calls already released the GIL for many CRUD operations, but
   metadata inspection, schema lookup, begin/commit/rollback and transactional
   point reads had gaps. Those blocking C++ sections now release it too. Python
   object conversion and callbacks retain/reacquire the GIL. TxHandles remain
   single-owner objects: do not concurrently operate on the same handle. This
   does NOT make pure Python CPU work parallel or support free-threaded Python.
4. Restore in the benchmark performed one durable update per key. Restore now
   uses bounded, acknowledged batches (at most 1000), outside timed workload
   measurement, then runs the configured integrity checks. Nexora/PostgreSQL use
   transactions; Mongo uses ordered bulk updates with matched-count checks. A
   failure stops restoration; there is no blind retry of unknown write outcomes.
5. The local Mongo configuration requests a **4GiB WiredTiger cache**, while the
   previous Nexora default was **128MiB block cache within a 256MiB cache/memtable
   budget**. The prior benchmark was not an equal-cache engine comparison. Cache
   misses/compaction plus Python generation/checksum overhead must be separated
   from native insert throughput. The common Python generation/checksum work was
   roughly .6s/100k rows in small probes: that client alone limits end-to-end
   throughput before considering storage. Parser caching does not accelerate the
   native BSON ingestion path used by the 100M lab.
6. The packaged API native extension was older than the root Release build. Both
   binaries were synchronized after rebuilding. Future deployments must copy the
   selected Release artifact into the package before starting a new process;
   rebuilding a different root file does not hot-upgrade a running interpreter.

## Measured, bounded checks

Raw artifacts are under `nexora-scale-lab/results/`:

| Probe | Before | After | Interpretation |
| --- | ---: | ---: | --- |
| Repeated parameterized syntax parsing/binding, three 200-call runs | median 369 calls/s | median 13,538 calls/s | about 36.6x in the parser only, NOT DB throughput |
| 2,000 durable updates of a non-indexed field | 780,680 WAL bytes | 201,560 WAL bytes | about 74.2% fewer WAL bytes |
| Same bounded durable update probe | 542.5 ops/s | initial 579.5; final repeat 529.5 ops/s | no repeatable latency improvement established; sync/host variation dominates |

See `parser_cache_ab.json`, `update_tuning_before.json`, and
`update_tuning_after.json`, and `update_tuning_final.json`. The byte reduction is
repeated in the final binary; the initial latency improvement was not reproduced.
The original 100M ingestion observation remains
41,447 records/s; no improved 100M rate has been established. Functional checks
cover cache bounds and parameter isolation, repeat authorization, real native
toggle equivalence/unique constraints, advisor opt-out/no-auto-DDL, administrator
API permissions, startup memory sizing, GIL release, integrity, rollback,
reopening and ownership-checked cleanup. A frontend typecheck/build and targeted
lint are required before shipping the generated dashboard assets.

## Query controls (trusted CLI or administrator session)

```sql
SHOW PERFORMANCE;
SET PERFORMANCE parser_cache = true;
SET PERFORMANCE clear_parser_cache = true;
SET PERFORMANCE read_cache_admission = true;
SET PERFORMANCE smart_indexes = true;
SET PERFORMANCE index_advisor = true;
SHOW INDEX ADVICE;
```

Use `false` to disable an option. Controls are strict booleans; unknown options
fail. They cannot run inside a transaction. Application tokens are denied these
global controls even with query/DDL scopes; HTTP preflights permissions before
executing any statement. The administrator REST endpoints are `GET /performance`
and `PUT /performance`, with the same option names as boolean JSON properties.

The Dashboard now shows controls, syntax-cache counters and storage cache
capacity/usage, hit/miss counters and pending compaction bytes. Mock mode does not
pretend to alter a real engine. Restart the API to load the new native module.

Important semantics:

- Parser cache is per loaded parser module/process and shared across its sessions;
  disabling clears it but does not cancel in-flight work. Exact SQL can contain
  literals, so cache eligibility/TTL/clear controls matter for sensitive workloads.
- Native options are per engine and runtime-only; defaults reset on restart.
  `read_cache_admission=false` prevents new data-block admission for point/query
  reads. Existing cached hits and internal metadata caching can still occur.
  It is NOT a cold-cache switch or a request to set cache capacity to zero.
- `smart_indexes=false` disables secondary index selection for query planning,
  potentially causing expensive scans. Primary-ID access, uniqueness/FK checks,
  index maintenance and data integrity remain enabled.
- Parser cache and index advisor default ON for new processes/engines, as
  requested for testing (2026-10-22). Explicit OFF remains respected. `SHOW`
  commands display state/advice; they are not settings or scheduled queries.
- The advisor is a local **deterministic heuristic, not an AI model**.
  It keeps at most 128 collection/field shapes, no predicate values or full SQL.
  After three eligible successful SELECT observations it suggests a single-field
  index if no leading-field index exists. Suggestions require manual review and
  confirmation in Query; they are not DDL, cost-based proof or automatic tuning.
  No remote model calls, credentials, charges or user-data uploads were added.
  Disabling it clears collected shapes. An actual model-backed advisor remains
  a separate, explicitly configured and tested future feature.

## Storage cache sizing

API startup supports `NEXORADB_ROCKS_MEMORY_MB` (default 256; 4..16384) and
`NEXORADB_ROCKS_CACHE_MB` (default 0 = half the budget; otherwise below total).
For example, a 1024MiB total/512MiB block-cache setup is explicit operator tuning,
not an automatic recommendation for every host. Defaults were NOT silently
raised. These are cache/memtable targets, not total RSS. Keep OS cache, allocator,
compaction, Python and cgroup headroom. No live capacity shrink is attempted.
The tuning pass does not relax the selected engine's durability mode.

The opt-in lab profile `config/compact-matched-cache-100m.json` requests 4096MiB
native block cache within a 5120MiB cache/memtable budget, retains durable WAL
acknowledgments, and keeps the worker's 8GiB cgroup limit. Preflight rejects
budgets leaving less than 512MiB headroom under that limit. The normal compact
profile stays at 256MiB/half-cache. Equal cache capacity is **not equal RSS,
identical storage layout, batch atomicity or a publication-quality protocol**.
The matched profile has not been stress-tested at 100M in this pass.

Use `scripts/run_matched_cache.sh --plan` to inspect it without loading data.
Any new matched-cache study importing Mongo must disclose that Mongo's archived
run used older instrumentation/serial loading. Retesting a new Nexora version
must use a new study and fresh dataset, not relabel old Nexora measurements.

The Python API driver also exposes bounded `insert_many(collection, documents,
idempotency_key=...)`: one parameterized batch request (1..1000 objects), not one
HTTP request per row. Values are never spliced into SQL. There is no automatic
splitting/retry, and native batch atomicity and unknown-outcome handling remain
explicit. For the lowest-overhead embedded path, `insert_many_bson` is still
available; the SQL/HTTP path is not claimed to have identical overhead.

TTL expiration is lazy on subsequent cache requests, not a secure erasure timer;
clear the parser cache explicitly when immediate eviction is needed. The loader
now rejects a requested different binary when another native build is loaded,
instead of silently ignoring it. Synchronize packaged/root Release artifacts
before starting; hot native type replacement is not supported.

## MongoDB / QuestDB: what is transferable?

MongoDB's WiredTiger has document-level concurrency, optimistic conflict
handling and compression. Its high-level insertMany API batches writes; secondary
indexes add write work, and bulk-build order matters. These support using bounded
bulk input, native parallel work and selective index maintenance here. They do
not prove a particular Nexora throughput target. See [WiredTiger architecture](https://www.mongodb.com/docs/manual/core/wiredtiger/),
[insertMany behavior](https://www.mongodb.com/docs/manual/reference/method/db.collection.insertmany/),
and [index write costs](https://www.mongodb.com/docs/manual/core/write-performance/).

QuestDB uses parallel per-connection/table WAL writers, a sequencer and an
append-friendly time-series/columnar storage architecture. WAL commit/ack and
asynchronous application/visibility are separate stages. A generic transactional
JSON store updating primary/secondary/unique/FK state is a different workload.
Nexora must not acknowledge an uncommitted in-memory queue and call that the same
durable benchmark. See [QuestDB storage engine](https://questdb.com/docs/architecture/storage-engine/)
and [WAL commit/application](https://questdb.com/docs/concepts/write-ahead-log/).
QuestDB's [client considerations](https://github.com/questdb/c-questdb-client/blob/main/doc/CONSIDERATIONS.md)
also distinguish local flush, server acknowledgment and query visibility. A
future typed append-only ingestion path, if desired, needs its own contract,
crash recovery, visibility/lag metrics and matched durability benchmark; it is
not implemented or claimed in this pass.

Python's GIL release is appropriate around native blocking/storage work, not
around Python object access. See [the CPython threading contract](https://docs.python.org/3.10/c-api/init.html#thread-state-and-the-global-interpreter-lock).
To explore higher throughput next, profile sustained compaction/cache misses and
concurrent native bulk writers with matched cache budgets, and separate producer
CPU from storage CPU. Do not start multiple embedded-engine server processes on
the same RocksDB directory to work around the GIL. The million-row/s target is
unproven and may require a distinct append-only storage path rather than more
cache or an AI switch.
