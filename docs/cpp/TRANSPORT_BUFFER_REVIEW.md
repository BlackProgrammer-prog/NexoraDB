# Transport, buffers and pagination review (2026-10-22)

Later follow-up: [ingestion slowdown tuning](INGESTION_SLOWDOWN.md) replaces
the initial immutable-BSON input copies with pinned synchronous input views.

Implemented and tested incrementally; not a security certification or a claim
of MongoDB/QuestDB throughput parity. No 100M run was started for this review.

## Changes

- Native `DocumentStream.next_ndjson_batch(limit)` constructs owned NDJSON
  bytes from the codec's bounded JSON page, without Python document decoding
  and re-encoding. It handles quoted delimiters, escapes, UTF-8 and nesting.
  Object/raw cursor modes cannot be mixed. Native I/O and encoding release
  the GIL; Python object creation happens only after reacquiring it.
- HTTP document streams and unprojected, zero-offset SELECT cursors select
  that path automatically. Projection/offset cursors retain the object path.
  Same wire records, authorization per batch, deadline, disconnect cleanup,
  cursor admission and ASGI send backpressure; no background prefetch queue.
- Page scans borrow the iterator value as `string_view` while matching and
  decoding it, eliminating the previous full stored-document copy. The view
  never survives `iterator->Next()` and is never exported to Python.
- Request admission accumulates into a bytearray, rather than keeping a list
  entry for every tiny/empty network fragment. Actual-body 2MiB and 10-second
  limits remain. Delivery creates owned immutable bytes and clears the buffer.
- Offset document pages now use native pages capped at 1MiB BEFORE Python
  decoding. Output is capped at approximately 4MiB encoded row data, max 1000
  rows, with a cooperative 30-second check, auth checks and finally-close.
  A single oversized document is rejected, not silently truncated. Each request
  is an independent snapshot; `nextSkip` does not promise cross-request consistency.
- Native page sessions are capped at 128 snapshots per engine, including
  embedded callers bypassing HTTP admission. Continuations still work when
  capacity is exhausted; completion/close releases capacity. Existing lazy
  five-minute expiry cleanup runs on the next page operation. HTTP cursor
  deadlines/cleanup bound normal stream retention to 120 seconds.

## Checked existing mechanisms

| Area | Current implementation / decision |
| --- | --- |
| Protocol | HTTP JSON requests, NDJSON streaming, pooled client; retain HTTPS/JWT/collection ACLs, bounded bodies and unknown-write-outcome handling. No new custom TCP or BSON wire protocol. |
| BSON | Versioned BSON storage and validated native `insert_many_bson` already exist. Length, UTF-8, nesting, duplicate fields, supported values, schema/index/FK constraints remain checked. Python immutable bytes are copied into owned native input before releasing GIL; no mutable borrowed-buffer shortcut. |
| Workers | RocksDB background flush/compaction jobs are bounded by CPU/settings (SSD default up to 4); graph scheduler has bounded workers/queue. HTTP sync/native work uses thread offload plus selected GIL release. No extra unbounded per-request native worker pool was added. |
| Buffers | Shared bounded RocksDB cache/memtables, native page byte budget, HTTP admission, bounded cursor batches. These are not an overall RSS cap; benchmark cgroup remains the hard containment boundary. |
| Pagination | Native snapshot/keyset continuation already exists and avoids repeated offset scans. Prefer `/documents/stream` or `/query/cursor` for deep sequential scans. Raw native continuation tokens are not exposed as cross-principal HTTP credentials. |

Use one engine-owning server process per RocksDB directory, with bounded
concurrent requests/threads; do not multiply Uvicorn processes against the
same writable RocksDB directory. More workers cannot remove storage or lock
bottlenecks. Native page-session operations still share a mutex; concurrent
scan lock contention and highly selective scans need separate profiling.
Cooperative HTTP deadlines do not forcibly interrupt an in-flight native scan.
General buffered `/query` (including JOIN) can still allocate a large native
result before its HTTP result check; this review does not solve that separate
execution-budget problem. Use supported single-SELECT cursors for large reads.

## Local A/B evidence

`scripts/benchmark_cursor_encoding.py --rows 10000 --repetitions 5 --output ...`
alternates both paths after warm-up over the same temporary store. Artifact:
`nexora-scale-lab/results/cursor_encoding_ab.json`.

Median Python roundtrip encoding: **56.61ms**; native NDJSON: **33.86ms**
(about **1.67x**, 40% less elapsed time). This measures warm local scan and
encoding ONLY, not HTTP/TLS, slow consumers, durability, insertion or 100M
throughput. Native BSON-to-JSON conversion, owned output construction and
network/TLS copies remain: this is copy reduction, not end-to-end zero-copy.

Ownership guidance: [RocksDB PinnableSlice](https://rocksdb.org/blog/2017/08/24/pinnableslice.html)
explains why borrowed data must remain pinned; [pybind11 object documentation](https://pybind11.readthedocs.io/en/stable/advanced/pycpp/object.html)
describes non-owning view lifetime requirements. No foreign memory is retained
or handed to asynchronous sends by this implementation.

Existing measurement artifacts are preserved. Core/binding edits invalidate
previously prepared source hashes; prepare a NEW continuation directory, never
rewrite an old manifest or relabel historical Nexora measurements as this build.
