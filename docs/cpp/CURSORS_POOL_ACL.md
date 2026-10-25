# Bounded SELECT cursors, pooled driver and collection ACLs

## Supported contract

`POST /api/v1/query/cursor?batch_size=128` accepts the ordinary query/parameters
body and streams NDJSON. Supports one SELECT with WHERE (compiled native
condition), nested projection, LIMIT, and SKIP. No implicit total-row cap.
Uses the native snapshot iterator, not `find_many` followed by slicing.
JOIN, multiple statements, transactions, mutations and graph algorithms are
rejected with 422, not silently buffered. This is not a general streaming
execution plan for every NexoraQL statement. Deep SKIP still scans skipped
matches; prefer consuming one live cursor rather than repeated offset reads.

Projection is compiled once per cursor. Native pages have a 1MiB JSON budget.
Python batches stop at 1MiB of encoded output or batch_size rows, whichever
comes first (one-row overshoot allowed, individual output record <=2MiB).
This avoids accumulating 1000 near-1MiB documents just because the row budget
is 1000. Encoded chunks are bytes, with no second event-loop encoding pass.
Python objects, RocksDB snapshots/cache, TLS and allocator overhead are extra;
these are not a process-RSS hard cap.

Each router admits at most eight live cursors and two per app ID. Excess gets
429 + Retry-After. A 120s overall cursor lifetime bounds slow-consumer snapshot
retention; final cleanup closes native iterators on success, exception,
cancellation, timeout and transport disconnect. A single synchronous native
call cannot be forcibly interrupted by an ASGI timeout; its worker is not
abandoned. The response includes no-store and X-Accel-Buffering:no; configure
proxy buffering/timeouts consistently. Tokens are checked for expiry,
revocation and collection read permission before each batch. A revoked token
does not retract data already transmitted.

Records are `{"document": {...}}` and finally `{"end":true}`. Missing end,
error records, or transport failure mean an incomplete result. Automatic
restart is forbidden: a restarted stream can observe a different snapshot.
There is no cross-process resume token or persisted cursor session.

## Pooled Python client

```python
from nexoradb.api import connect

with connect(url="https://database.example", token=token,
             max_connections=8, pool_timeout=2, timeout=30) as client:
    with client.cursor(
        "SELECT customer.name, total FROM orders WHERE total >= $minimum;",
        {"minimum": 100}, batch_size=128,
    ) as rows:
        for row in rows:
            consume(row)
    print(client.pool_stats())
```

HTTPX maintains a real connection pool; the real-TCP test verifies multiple
requests reuse the same peer connection. Defaults: maximum 16 connections/
inflight operations, up to eight idle connections, 30s keepalive expiry, 2s
acquisition timeout. A semaphore also bounds HTTP/2 concurrent operations,
not merely TCP connections. Acquisition failure raises NexoraDBPoolBusy before
sending. No automatic query/write retries. Cursor exit releases the response,
connection lease and parser buffer even on early stop. Client metrics have
fixed labels/counters, not unbounded per-query lists.

Create one long-lived client per process; do not create one per request. The
sync client supports sharing among threads, but must not be inherited across
fork or used directly inside an async event-loop handler. A native async
client is not provided by this change. Close after callers/cursors drain;
concurrent close and active requests is not a supported lifecycle contract.
Always use nested context managers for cursor early exit.

HTTP/2 is opt-in (`http2=True`) and needs a supporting TLS endpoint; negotiation
can fall back to HTTP/1.1. HTTP/3 is not provided by HTTPX here. TLS certificate
and hostname verification cannot be disabled by this API. Supply a verified
SSLContext via `ssl_context=` for private CA/mTLS. Proxy environment variables
are not implicitly trusted, and redirects are not followed.

## Explicit, deny-by-default collection permissions

Admin `POST /apps/tokens` now accepts:

```json
{
  "appId": "orders-service",
  "scopes": ["query:execute", "documents:read", "documents:write"],
  "collections": {"orders": ["read", "write"]},
  "expiresInSeconds": 3600
}
```

New tokens default to an empty resource grant set, not wildcard access.
read/write/manage are independent permissions (manage covers schema/DDL).
Exact collection names only; no implicit wildcard/inheritance. The statement
scope AND collection permission must both allow the operation. A JOIN needs
read grants for both collections; foreign-key creation needs read on the
reference collection. Every statement is authorized before any statement
executes, including mixed requests. Direct document pagination/export and
query cursors use the same policy. Internal reserved collections are denied.

Legacy tokens without collection claims work only outside production and
must be reissued before production use. Production rejects broad legacy app
tokens. Resource-restricted app tokens cannot use global discovery, graph or
system operations that could indirectly expose unrelated data. Privileged
admin endpoints remain separately admin-authenticated; they are not tenant
endpoints. Graph/row/field-level ACLs are **not** implemented here: fail-closed
graph access is not a claim that graph tenant isolation is feature-complete.
Reissuing tokens changes identity; reconcile outstanding idempotent requests
first. New resource grants participate in the receipt identity; legacy receipt
identity is preserved for development-mode legacy tokens.

## Network soak and validation

```bash
NexoraDB/.venv/bin/python scripts/benchmark_network_soak.py --seconds 600
```

Creates/discards a private fixture, runs real loopback HTTP/1.1 with signed,
revocable tokens, two pooled client workers and native RocksDB reads. Mix is
90% COUNT and 10% filtered/projected SELECT cursors (100 documents). Samples
are bounded to the latest 8192 latencies; counters cover the full run. Reports
joint server+client RSS and FD counts. Fails on any error or >64MiB final RSS
growth by default. This engineering bound is not a leak proof or RSS budget;
warm-up/allocator growth and native block cache count in the same process.
The full performance workflow runs the 600s soak weekly/manual full, retaining
the JSON artifact. Remote workflow execution still requires runner setup.

Local short validation (60.24s): 21,721 successful operations, zero errors,
360.6 operations/s, recent P99 13.97ms; RSS ~83.7MB ->95.0MB. This overlapped
the longer local soak and is not an isolated throughput comparison.

The 10-minute run completed: 219,167 successful operations (197,252 COUNT,
21,915 cursor), zero errors, 365.23 operations/s; recent P99 15.02ms. Joint
RSS was 84,254,720 ->95,789,056 bytes, with the observed peak equal to final
RSS. FD counts were 20 before client startup and 24 with two active keepalive
client/server socket pairs. This run began before final byte-batch and pool
metric refinements. A subsequent 60s run and a final 10s smoke passed; the
final smoke released all 3550 leases, with zero active leases/timeouts after
draining. These are workload-specific results, not a full final-release soak
or a proof that no resource leak exists.

Neither this loopback soak nor unit tests establish WAN/TLS performance,
write-heavy crash correctness, multi-host fairness, HTTP/2/3 superiority,
industrial SLA or independent security certification. The old audit's pooled
driver/filtered-SELECT gaps are partially addressed by this implementation;
JOIN cursors and transactional idempotency receipts remain separate work.

Final local validation: 59 Python API/security/driver tests and two native
recovery tests passed. The wheel built successfully and passed `twine check`.
Remote workflows have not been claimed as executed or green.

## Why no machine learning in the control path

Admission, authorization and memory bounds must be predictable and testable.
There is no dataset or baseline demonstrating that an ML controller improves
this workload. A static bounded pool with observable wait/timeout counters is
the initial policy. Any future adaptive/AIMD or learned controller must stay
within hard limits, run in shadow mode first, and beat the fixed policy under
bursty load/failures before controlling production resources. An ML model must
never override authorization, durability or resource limits.

References: [HTTPX pools](https://www.python-httpx.org/advanced/clients/),
[resource limits](https://www.python-httpx.org/advanced/resource-limits/),
[timeouts](https://www.python-httpx.org/advanced/timeouts/),
[Neo4j batching](https://neo4j.com/docs/python-manual/current/performance/).
