# Data-path performance and security audit (2026-10-21)

Latest incremental follow-up: [transport/buffer review](TRANSPORT_BUFFER_REVIEW.md)
adds native NDJSON encoding, bounded offset pages and snapshot admission.

Follow-up implementation and evidence: [SELECT cursors, pooled client and
collection ACLs](CURSORS_POOL_ACL.md). Historical findings below describe the
initial audit; that follow-up partially addresses the listed gaps without
claiming general JOIN cursors or industrial certification.

## Conclusion

The embedded engine has useful production-oriented primitives, but this audit
does **not** establish performance, security, availability, or feature parity
with PostgreSQL, SQL Server, or Neo4j. No side-by-side benchmark, independent
penetration test, distributed failover test, or target-hardware SLA was run.
The external API/driver is an alpha interface, not an industrial certification.

## Read/write path inspected

```text
RocksDB snapshot / iterator / point Get / candidate MultiGet
  -> C++ DocumentCodec (versioned BSON envelope -> JSON)
  -> pybind11 (owned strings/results; selected calls release GIL)
  -> NexoraQL Python executor (JSON decode, projection, result normalization)
  -> FastAPI (buffered JSON, or bounded document NDJSON batches)
  -> ASGI server / trusted TLS reverse proxy / network
  -> Python driver / consuming application
```

Relevant implementations: `core/DocEngine.cpp`, `core/DocumentCodec.cpp`,
`bindings/nexoradb_bind.cpp`, `nexoradb/parser/semantic.py`,
`nexoradb_admin/query_runner.py`, `nexoradb/api/app.py`, and `api/driver.py`.

- Indexed reads use candidate MultiGet; scan paths set iterator bounds and
  snapshot-local options. Full SELECT still materializes JSON in the core.
- The Python executor decodes that JSON and applies projection in Python;
  this is not zero-copy or projection pushdown.
- The compatibility response duplicates information in `rows` and `raw`.
  `include_raw=false` now removes the raw payload; `statementErrors` preserves
  errors even when raw is absent. Clients must inspect statement errors.
- Native `iter_documents` reads bounded pages from a snapshot. HTTP streaming
  now performs worker transfer **and encoding per batch**, not per document.
  The stream is a collection export, not a general WHERE/JOIN query cursor.
- Buffered response byte checks happen after native execution. They prevent
  oversized HTTP output, not a large native allocation. Query/result execution
  budgets and a general query cursor remain significant work.

## Measured local evidence

Run: `NexoraDB/.venv/bin/python scripts/benchmark_data_path.py --rows 5000 --repetitions 5`.
WSL Linux x86_64, Release 0.3.0, GCC 11.4, Python 3.10.12, RocksDB 8.5.3.
Warm reads, one collection, 5000 documents with numeric ID/value and 64-byte
text. One warm-up excluded. Timings include tracemalloc overhead.

| Stage | Median ms after batching | Python allocation peak bytes |
|---|---:|---:|
| Native query returning JSON to Python | 10.51 | 508878 |
| Native query plus Python JSON decode | 15.87 | 2672629 |
| ASGI query, rows + raw | 62.01 | 6445379 |
| ASGI query, no raw | 53.26 | 6440679 |
| ASGI document stream | 52.03 | 1460090 |

Before the batch handoff fix the same stream probe measured 597.50ms median
(range 594.91..606.72); after it measured 52.03ms (51.56..53.08).
This ~11.5x local improvement is **not** an end-to-end network claim.
TestClient buffers streaming output and excludes actual TLS/network/client
pooling/authentication; native memory/RSS is not counted by tracemalloc.
Removing raw reduced wire work here but did not materially reduce Python
allocation peak. Do not present these numbers as industrial throughput.

Industrial reference patterns: [Neo4j lazy fetch/batching](https://neo4j.com/docs/python-manual/current/performance/),
[PostgreSQL portals and bounded Execute](https://www.postgresql.org/docs/17/protocol-flow.html),
and [HTTP retry rules](https://www.rfc-editor.org/rfc/rfc9110.html#section-9.2.2).

## Security changes and operational requirements

- HTTP body admission counts actual bytes, including chunked requests, before
  JSON parsing: 2MiB maximum and 10s body deadline. Concurrent application
  requests default to 16; streaming holds its slot until cleanup. This does
  not replace proxy header, connection, rate and slow-reader limits.
- Production FastAPI rejects non-HTTPS ASGI requests. Terminate TLS at a
  trusted proxy and configure forwarded-header trust for that proxy **only**;
  never trust arbitrary client `X-Forwarded-Proto`. Socket.IO is wrapped
  outside FastAPI, so enforce TLS/access controls at the proxy for it too.
- CLI binds loopback by default. Remote driver URLs require HTTPS; TLS uses
  Python's normal certificate verification. Loopback HTTP remains for local
  development. Bearer tokens must not be logged. The driver refuses redirects
  so credentials/writes cannot be forwarded automatically to another origin.
- Driver responses are byte-bounded and POST transport failures raise
  `NexoraDBOutcomeUnknown`. No automatic write retry is enabled.
- Lazy engine/graph initialization and parser loading are serialized, avoiding
  duplicate startup or exposure of a partially imported parser under load.
- Existing parameter binding, constant-time signatures, managed-token
  revocation and statement scopes remain in place. Read/write scopes are
  broad, **not per-tenant collection/row authorization**. Do not expose one
  database as a hostile multi-tenant service without additional ACL design.
- Responses use no-store by default and nosniff through admission middleware.
  These headers do not constitute encryption at rest or a security audit.

## Duplicate requests: explicit contract

External `POST /api/v1/query` accepts `Idempotency-Key` (1..128 characters,
letters/digits/underscore/dot/colon/hyphen). The driver exposes it as:

```python
client.execute(query, parameters, idempotency_key="stable-operation-uuid")
```

Persist the key in the calling application **before** sending a business
operation, reuse exactly that key/body/token on retry, and never mint a new
key merely because of timeout. Requests without a key retain normal semantics
and can repeat writes. Dashboard mutation endpoints do not gain idempotency
implicitly. The application must inspect statementErrors, not just HTTP 200.

The private `DB_PATH/api_requests.sqlite3` journal uses SQLite FULL sync.
It durably reserves a principal-bound key before execution, records a payload
hash, then saves the completed response after document flush and graph
checkpoints. Completed duplicates replay the saved response without execution;
changed payloads return 409. Auth/revocation are checked before replay.
Identity includes app ID, token ID and current scopes: changing tokens is
**not** a valid way to continue the same receipt. Do not delete receipts to
force retries. Cached responses contain data; protect the directory/backups.

Concurrent, interrupted, failed, or crash-ambiguous reservations return 409;
they are never automatically expired or reexecuted. The operator must reconcile
the actual database/business state. Errors after execution or receipt persistence
failures can leave such a reservation. No claim of atomic exactly-once commit
is made: SQLite and RocksDB/graph are separate commit domains. Grouped queries
are not automatically transactions, and cross document/graph atomicity is
not introduced. The conservative durability barriers add write cost.

The journal admits at most 10,000 identities and ~64MiB SQLite pages (sidecar
and transient storage overhead excluded); capacity failures fail closed. This
is a bounded alpha implementation, **not** a scalable retention/reconciliation
service. Future industrial work should store receipts with the document
transaction in RocksDB, with explicit graph outbox semantics and audited
retention/reconciliation APIs. Active/unknown keys must never be evicted.

Offline admin backup now snapshots an existing request journal through the
SQLite backup API under the same quiescent-source contract and includes it in
bundle checksums. Restore retains receipts. Stop the whole API, not only
writers. Old backups without receipts cannot preserve a newer retry history;
invalidate outstanding client operations/reconcile before resuming. Rollback
must restore the matching data **and** journal, never just one component.

## Validation and remaining gates

45 Python API/security tests passed, including concurrent duplicates, process
exit after reservation, restart/replay, changed payload, principal isolation,
revocation before replay, body limits and TLS/redirect/timeout checks.
Two existing native recovery integration tests passed. An additional local
native fixture backup/restore probe verified receipt replay after restoration.
All 128 Release C++ tests passed again. A native HTTP INSERT probe without a
document ID sent the same key twice and verified only one document existed.
These do not replace disk-full/permission/power-loss stress of the new journal.

Before claiming industrial readiness: implement true filtered/projected query
cursors, pooled sync/async drivers with lazy streaming consumption, transactional
receipts/outbox, per-principal rate/fairness and ACLs, execution deadlines/native
cancellation, mixed sustained workload benchmarks (TLS and real clients),
slow-consumer/disconnect resource tests, and an independent security review.
HTTP/2/3 controlled network measurements remain pending; see
`HTTP_API_CAPACITY.md`. Nothing here establishes SQL Server/Neo4j/PostgreSQL parity.
