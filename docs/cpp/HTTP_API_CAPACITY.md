# HTTP API capacity and transport experiments

Filtered/projected SELECT streaming and pooled driver details are now in
[CURSORS_POOL_ACL.md](CURSORS_POOL_ACL.md); the collection-export limitation
below refers to the original GET export endpoint, not the new query cursor.

See [the data-path audit](DATA_PATH_AUDIT.md) for measured stage costs,
duplicate-request semantics, TLS requirements and remaining production gaps.
Admission now also enforces a 2MiB body limit and a 10s body deadline.
Production FastAPI requires HTTPS (trusted TLS proxy configuration required).

## Admission and monitoring

Both FastAPI applications use `NEXORADB_API_MAX_INFLIGHT` (default 16,
range 1..1024). Capacity is per application/process, not global across replicas.
There is no application waiting queue. Excess HTTP requests receive 503 and
`Retry-After: 1` before endpoint/auth execution. A slot remains held through
the entire ASGI response, including streaming and disconnect cleanup.
All HTTP routes, including health/static routes, share the cap. Socket.IO
connections outside the FastAPI application are not covered by this limit.
Configure proxy connection/body/time limits as well; this is not a hard RSS
budget, rate limiter, or replacement for native graph job admission.

Monitoring moves the complete synchronous engine/filesystem sampling block
to an AnyIO worker with a dedicated capacity limiter of one. Cancellation
does not abandon an ongoing native call. It cannot interrupt a stuck syscall.

## Bounded reads

Application bearer tokens require `documents:read` for these routes, including
managed-token revocation validation:

- `GET /api/v1/collections/{collection}/documents?limit=100&skip=0`:
  1..1000 rows; skip 0..1,000,000. Returns `{rows, nextSkip}`. A full final
  page can require one extra empty request. Pages are independent offset
  reads: concurrent writes can change membership/order; no cross-request
  snapshot guarantee. Responses over 4MiB fail with 413 **after** fetching.
- `GET /api/v1/collections/{collection}/documents/stream?page_size=128`:
  lazy native document iterator, 1MiB native page budget, at most 1000 rows
  per page, with no implicit total-row cap. Optional `limit` specifies the
  caller's desired total, independently of page size. Each NDJSON record is
  `{"document": ...}`; successful completion has `{"end":true}`. Completion
  means the requested limit or EOF, not necessarily collection exhaustion.
  Consumers must check the end record; a later failure produces an error
  record or a disconnected/incomplete stream. The iterator closes on success,
  exception, or disconnect. Native reads and cleanup run off the event loop.

HTTP query execution preserves the user's SELECT LIMIT, including no LIMIT;
there is no fixed 1000-row total cap or silent truncation. The 1000-row limit
on a document page is a batch limit, not a collection/export limit.
Native embedded APIs are unchanged. The 16MiB query response guard
is checked after execution, so it is not an execution-memory guarantee;
multi-statement, graph, aggregation and individual large documents still
require engine/proxy budgets. Existing dashboard list routes retain their
response shape; clients should migrate large reads to the new routes.

The document stream currently exports a collection; it is **not** a general
SQL cursor for WHERE/JOIN/projection. Large arbitrary SELECT results exceeding
the buffered byte limit still need a future query-cursor execution path;
do not replace a filtered SELECT with an unfiltered export implicitly.

Industrial precedent: MongoDB separates cursor `batchSize` from total LIMIT,
PostgreSQL uses DECLARE/FETCH cursors, and Elasticsearch recommends
`search_after` with PIT for deep, consistent pagination:
[MongoDB](https://www.mongodb.com/docs/manual/reference/method/cursor.batchsize/),
[PostgreSQL](https://www.postgresql.org/docs/current/sql-declare.html),
[Elasticsearch](https://www.elastic.co/docs/reference/elasticsearch/rest-apis/paginate-search-results).

## HTTP/1.1 vs HTTP/2 vs HTTP/3 benchmark

`scripts/benchmark_http.py` probes an actual authenticated query endpoint,
requires the negotiated protocol to match, and rejects non-200 responses.
Use a **read-only** query/token and an isolated test database. Keep the token
in `NEXORA_BENCH_TOKEN`, never arguments or committed files. Use the same
dataset/query/TLS proxy/backend settings for all protocols; configure both
HTTP/2 and HTTP/3 support at the test proxy and verify curl's feature list.

Example (repeat at least five times per protocol/condition and retain JSONL):

```bash
python3 scripts/benchmark_http.py --url https://test-host/api/v1/query \
  --protocol 1.1 --samples 1000 --network-label baseline
# Repeat with --protocol 2 and --protocol 3.
```

Each sample uses a fresh curl process/connection, including handshake. This
measures cold request latency, not pooled throughput or an async driver.
It verifies HTTP status/protocol, not application-level statement success;
validate the selected read query separately before measurements.

For controlled network conditions use a disposable Linux VM/container or
network namespace, never the WSL default interface or a production host.
Install `iproute2` and `CAP_NET_ADMIN` in that isolated environment. Apply:

```bash
# INSIDE the disposable client network namespace only:
tc qdisc replace dev eth0 root netem delay 25ms loss 0.1%
tc -s qdisc show dev eth0
```

Repeat baseline, 25ms/0%, 25ms/0.1%, and 50ms/1% with the same MTU and
bandwidth. This rule adds delay/loss only on client egress, **not symmetric
RTT**. For symmetric conditions configure both disposable endpoints and
record both rules plus measured RTT, qdisc drop counters, seeds (if supported),
curl/server versions, CPU and handshake/connection policy. Run protocols in
alternating order; compare distributions, errors and CPU, not just averages.
Restore/destroy the isolated environment afterwards.

No transport winner or HTTP/3 performance improvement is claimed here.
The current WSL curl 7.81.0 has HTTP/2 but no HTTP/3; Hypercorn/aioquic are
not installed. No privileged network shaping was applied to shared WSL.
Actual controlled HTTP/2 and HTTP/3 measurements remain an environment gate.
