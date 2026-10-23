# Safe batching, binary requests and concurrency (2026-10-22)

This follow-up fixes the transaction-read GIL abort and adds opt-in APIs.
No 100M experiment was run; historical MongoDB/Nexora measurements remain
unchanged. New source/binary hashes require a fresh prepared study directory.

## Implemented

- `find_by_id_tx` had two consecutive `gil_scoped_release` guards. A bounded
  subprocess reproduced a fatal `PyEval_SaveThread` abort (exit -6). Removed
  the duplicate guard and extended the subprocess regression to execute the
  previously uncovered transaction-read path.
- `insert_many_bson_buffer(collection, immutable_bytes)` accepts concatenated
  BSON documents (each has its own BSON little-endian length). One pinned
  immutable buffer replaces a Python list of byte objects. Framing is checked
  before mutation, and every document is still validated before atomic commit.
  Limits: 64MiB per native buffer, 10000 documents, codec's 16MiB per document.
  Mutable bytearray input is rejected. Views never escape the synchronous call.
  Packing an already-built list adds a join/copy; no unconditional speedup is
  claimed. Existing list-based input remains available and the benchmark has
  not silently switched ingestion APIs.
- Native pagers now hold the registry mutex only for session lookup, expiry,
  registration and removal. Each session has its own scan mutex. Shared session
  and snapshot ownership keep in-flight reads valid during concurrent close.
  Independent cursors can scan concurrently. Close revokes continuation lookup;
  an already-running page may finish, retaining its snapshot until return.
  The registry remains capped at 128 sessions; in-flight calls temporarily pin
  snapshots even after registry removal. HTTP's own admission/cursor budgets
  remain essential. Same-cursor concurrent calls are serialized, not a promise
  of exactly-once token consumption. Tokens remain an embedded API, not public
  cross-user HTTP credentials.
- Native `operation_metrics()` exposes `write_path`: configured durable ACK,
  WAL enablement, unordered/pipelined settings, effective write-buffer size,
  background jobs and optional RocksDB WAL-sync/leader/follower counters.
  `load-engine.jsonl` captures this alongside prior load diagnostics.
  These are process/engine lifetime statistics, not an exact per-user fsync
  count or proof of group-commit benefit. Compare deltas and report when
  statistics are unavailable.

## Binary HTTP request protocol, version 1

`POST /api/v1/query/bson`, `Content-Type: application/bson`, accepts ONE BSON
object with the same `query`, `parameters`, `include_raw` envelope as JSON.
Response remains the existing JSON `QueryExecuteResponse` (including statement
errors). This is HTTP content encoding, not a custom TCP server, wire
compatibility with MongoDB, zero-copy network I/O or server-side prepared SQL.

The endpoint calls the SAME query executor and replay journal as `/query`:
JWT, revocation, scopes, collection grants, graph integration, transaction
handling and idempotency checks are not bypassed. Same logical JSON/BSON
envelopes may replay with the same principal and Idempotency-Key. Reusing a key
with changed content is a conflict. Durable journal barriers and unknown-outcome
handling are unchanged. No automatic retries or batch splitting were introduced.

Production still requires HTTPS. Admission enforces 2MiB actual request bytes
and body timeout before parsing, with a second endpoint length check. A rebuilt
native decoder validates BSON lengths, UTF-8, nesting, duplicate keys and finite
numbers. Binary query envelopes are restricted to JSON-compatible scalars,
objects and canonical arrays: dates, binary, ObjectIds, code and Decimal128 are
rejected rather than silently coerced. No compression/decompression protocol
was added. BSON validation/decode and query execution run in a worker, not on
the ASGI event loop. The decode currently passes through JSON internally, so
no binary transport latency/throughput improvement is claimed without A/B.

Client-side encoder dependency is optional: `pip install 'nexoradb[binary]'`.
The server uses the native libbson codec; it does not need PyMongo to decode.

```python
client.insert_many('items', [{'_id': 'one', 'value': 1}],
                   binary=True, idempotency_key='import-001')
client.execute('SELECT * FROM items;', binary=True)
```

## Async and lifetime contract

`await client.aexecute(query, parameters, binary=True, ...)` is an explicit
thread-backed convenience over the existing pooled synchronous client. It
does not block the event loop and retains the client's concurrency limit.
AnyIO `abandon_on_cancel=False` waits for an in-flight worker to finish rather
than orphaning it. Cancellation is NOT database rollback and never authorizes
blind retry of a write; retain its idempotency key and reconcile unknown results.
Close the client only after all users/tasks have finished. This is not a native
async HTTP client or an embedded transaction-handle scheduler.

Embedded `TxHandle` remains single-owner/single-thread; never run begin, reads,
commit and handle destruction in unrelated `to_thread` calls. Wrap the whole
transaction in one worker function. Releasing GIL permits concurrency but
neither makes a RocksDB transaction thread-safe nor changes its isolation level.

## Group commit, WAL and counter: deliberately preserved guarantees

RocksDB already groups compatible concurrent writers in its write path.
This revision does not add an application delay queue, async WAL acknowledgment,
`unordered_write`, disabled WAL, weakened checksums or a new transaction policy.
The benchmark remains durable; the API's separately configured/default native
durability is reported, not silently relabeled durable. Request-journal replay
barriers remain the stronger explicit receipt path.

The native 100M loader still uses one writer and atomic 1000-document batches,
already amortizing commits. Extra group commit is not an automatic improvement
for that serial pipeline. The per-collection counter's GetForUpdate remains a
contention candidate for concurrent inserts/deletes. Removing its lock or
updating asynchronously would risk wrong COUNT and crash consistency. Sharded
counters require a versioned migration, snapshot-consistent summation, rollback
and compatibility tests; that storage-format change was NOT rushed into this
revision. DDL/unique/FK locks were not removed.

Tests cover concurrent durable paired inserts versus snapshot SELECT, parallel
pagers, scan/close races, packed framing/rollback/ownership, abrupt-process-exit
recovery of an acknowledged batch, binary authentication/ACL/replay/revocation,
duplicate command fields, non-JSON types, async cancellation and GIL regression.
Process exit recovery is not a power-cut or faulty-device durability test.

## Small packing-cost probe

`scripts/benchmark_ingest_buffers.py --rows 100000 --repetitions 3
--buffers-mib 64 --input-api list|packed --output ...` measured median
~77,112 records/s for existing byte lists versus ~74,363/s for joining those
lists into a packed buffer (~3.6% lower in this small probe). Runs were separate,
not a randomized crossover, so this does not establish a general regression.
Artifacts: `nexora-scale-lab/results/safe_batch_{list,packed}_probe.json`.
The existing list-based benchmark path remains the default; the packed API is
for callers already holding contiguous BSON, not a claim that copying existing
lists makes ingestion faster. No wire-protocol speedup has been measured.
