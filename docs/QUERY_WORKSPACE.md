# Query workspace: editor, diagnostics and bounded results

Available in NexoraDB 0.5.2.

The Textual terminal REPL now uses these same parser and execution endpoints.
See [TUI_QUERY_WORKSPACE.md](TUI_QUERY_WORKSPACE.md) for terminal key bindings,
bounded previews, cancellation and compatibility tests.

## Existing capabilities and integration

The console already accepted multiline text. The Python/Lark parser already
reported line/column diagnostics, and the C++ engine already provided bounded
document pages, retained snapshots, execution scopes and native NDJSON batches.
The external SDK already exposed `/api/v1/query/cursor` as a streaming SELECT.
The native engine now also exposes `iter_join_documents`: bounded, cancellable
LOOKUP JOIN pages sharing one RocksDB snapshot for source and lookup targets.

The console now has:

- A CodeMirror editor with line numbers, NexoraQL lexical highlighting,
  indentation, bracket matching and undo/redo. Enter inserts a newline;
  Ctrl/Cmd+Enter executes the full document. Escape then Tab leaves the editor.
  Editor input is capped at 200,000 UTF-16 code units; server request/parser
  limits still apply independently.
- Debounced, abortable server diagnostics using the actual NexoraQL parser and
  semantic validator. Validation never executes the submitted SQL. It does not
  verify database schema, existence of collections, or success of later execution.
- Inline markers for located errors and a textual list of diagnostics. Unicode
  Python columns are translated to CodeMirror UTF-16 offsets. Leading blank lines
  and comments are preserved; the optional inserted semicolon does not shift EOF
  diagnostics beyond the original document. Semantic errors without source
  coordinates remain global messages rather than invented marker locations.
- Buffered execution for all supported statements, a live streaming preview, and
  explicit forward-only server cursor pages. Statement errors in an HTTP 200
  buffered response are now shown as failures, including the possibility that
  earlier writes already committed.

Editor and language code are loaded with the query page, not the sign-in entry.
The entry bundle remains approximately 244 kB; the query-page chunk including
CodeMirror is approximately 404 kB. The existing bundle budget continues to pass.

## Result modes

| Mode | Intended use | Client retention | Server behavior |
| --- | --- | --- | --- |
| Buffered | Scripts, mutations, graph commands and ordinary queries | Existing bounded response/table preview | Existing execute endpoint; no automatic fallback from other modes |
| Streaming preview | Large SELECT/JOIN results or supported scripts | Up to 1,000 latest records within 8 MiB encoded data; table renders 50 at a time | Incremental reads; side-effecting scripts finish in a bounded disk spool before delivery; NDJSON with backpressure |
| Server cursor | Browse results one page at a time | Current server page only, 100 records by default | Same program execution contract, forward-only pages; close/expiry frees the reader |

Streaming and server cursors support all statement types implemented by the
NexoraQL executor, subject to existing language, transaction and resource limits.
Programs are parsed and semantically checked in full before any execution.
They accept up to 256 statements and eight distinct LOOKUP JOIN targets per
SELECT. Single SELECT responses preserve the existing row protocol. Scripts
return records tagged with `statement` (1-based), `statementType`, and `row`;
each statement also has a `receipt` summary. Empty result sets have receipts.
`totalRows` counts delivered records, including summaries, rather than only
documents. The same global browser preview budget applies across the script.

Each SELECT retains its own snapshot across pages, including its lookup targets.
Separate SELECT statements use separate snapshots (read committed); scripts
are not implicitly atomic. Existing LOOKUP semantics are preserved: scalar
equality, inner lookup, first matching target, not relational fan-out. The first
target is available as `__joined__`; multiple targets are additionally available
as `__joins__.<collection>`. Later stages can reference those paths. Repeated
target names require aliases, which the current grammar does not support, and
are rejected. Index the lookup field (prefer a unique key when target selection
must be unambiguous). Unindexed lookups may scan the target per source document.
After DDL changes, retained JOIN snapshots use data scans rather than indexes
whose entries may have been created after the snapshot.

Read-only programs advance lazily. Aggregates and graph algorithms retain their
existing blocking/native resource bounds; they are not advertised as incremental
operators. Programs containing writes, DDL, transactions, jobs or unknown side
effects execute completely before their first result page is delivered. SELECT
results in those programs still use native iterators, not full materialization.
Results are written to private files (0700 directory, 0600 exclusive files), with
64 MiB per program, 256 MiB per process and a 2 MiB record limit. Global capacity
is reserved before execution. There is no open transaction between HTTP pages.
Spools expire 120 seconds after preparation; expiry/restart never reruns writes.

Side-effecting programs require `Idempotency-Key` (1–128 ASCII letters, digits,
`_ . : -`). The browser creates and displays one key per explicit run. Reopening
with the same principal/key/query/parameters replays prepared results without
executing again; conflicting payloads return 409. The durable journal does not
expire reservations automatically. A process failure between RocksDB work and
journal completion leaves an unknown outcome (409 on retry), requiring operator
reconciliation. This is deliberately not a claim of crash-atomic exactly-once
execution. Expired results return 410, with recorded receipts when available.
`GET /query/requests/{key}` exposes admin-owned completion status and receipts.
Scripts stop at the first failed statement; earlier committed work is preserved.
Transaction receipts distinguish committed and rolled-back staged operations.
The final execution receipt must be read before assuming a multi-statement write
program completed successfully; stopping delivery does not undo committed work.

Filtering, parameters, projection, LIMIT and SKIP preserve existing NexoraQL
semantics. SKIP is capped at 1,000,000 and remains O(skip); prefer a selective
predicate. ORDER BY/GROUP BY are not added to the language by this editor change.
Unsupported query shapes fail explicitly rather than executing a buffered fallback.
Inside explicit transactions SELECT/UPDATE/DELETE still require an exact `_id`
predicate; transactional JOIN and transactional graph projection remain engine
limitations and are rejected during preflight. Graph-enabled write sessions
retain the existing projection hooks; transaction writes in those sessions are
rejected rather than silently skipping graph maintenance.

Streaming is a preview, not a complete export held in browser RAM. Earlier rows
are deliberately discarded when either the row or encoded-byte budget is reached.
The ring buffer immediately releases references to evicted row objects, and
columns are derived only from retained rows. The byte budget does not measure
JavaScript object overhead or total browser RSS. Use the SDK streaming
cursor to consume or save all rows without materializing them:

```python
with client.cursor("SELECT * FROM items;") as rows:
    for row in rows:
        process(row)
```

Stop aborts the HTTP request and closes a known server cursor. Unmount, replacement
queries, EOF and service shutdown also close resources. If an open response is
lost before the browser knows its ID, the server's reaper is the fallback.
Stopping buffered execution does not undo writes that already committed.

## Admin API

All endpoints require a currently active admin account, a valid bearer token and
HTTPS in production. Tokens remain in tab memory, not browser storage or cookies.

### Parser-only validation

`POST /query/validate`

```json
{"query":"SELECT value\nFROM items LIMIT 100;","parameters":{}}
```

Returns `valid`, `diagnostics`, `statementCount` and `cursorSupported`. Responses
also report `executionStrategy` (`incremental` or `prepared`) and
`requiresIdempotencyKey`. Database/graph availability remains an execution check.
Diagnostics have a message, severity, and nullable 1-based line/column coordinates. Invalid
queries return a normal validation response with `valid: false`; authentication,
body limits and admission failures still use their HTTP error statuses.

### Streaming

`POST /query/stream` with `query`, optional `parameters`, and `pageSize` (1–500).
The response is `application/x-ndjson`, `Cache-Control: no-store`, with
`X-Accel-Buffering: no`. Proxies must allow streaming and disable buffering.
The external `/api/v1/query/cursor` uses the same program execution contract;
application scopes and collection grants are checked for the entire program
before execution and rechecked on each delivery batch, including JOIN targets.
The Python driver's `cursor(..., idempotency_key='your-key')` accepts the same
explicit key for write scripts. Use `client.query_request_status('your-key')`
(`/api/v1/query/requests/{key}`) to inspect application-owned receipts. Status
lookups expose no query data and cannot inspect another application's keys.
Single SELECTs without projection/offset retain the native NDJSON fast path;
other programs decode only bounded native pages and bounded result records.

```jsonl
{"document":{"_id":"one","value":1}}
{"document":{"_id":"two","value":2}}
{"end":true}
```

Success requires the terminal `end` frame. A later server error sends an `error`
frame without `end`; a disconnected/truncated stream is also incomplete. The
console retains any already displayed rows as an explicitly incomplete preview.
The client decodes incremental UTF-8, limits a record to 2 MiB, and awaits batch
consumers before reading more. It does not apply the buffered 8 MiB whole-response
cap to successful streams; error responses are capped at 64 KiB.

### Stateful server cursor

1. `POST /query/cursors` with `query`, `parameters`, `pageSize` opens a cursor and
   returns its first page.
2. `POST /query/cursors/{cursorId}/next` with the next `sequence` and optional
   `pageSize` fetches one more page.
3. `DELETE /query/cursors/{cursorId}` closes it; repeated closes are harmless.

Page fields: `cursorId` (null at EOF), `rows`, `columns`, `hasMore`, `sequence`,
`totalRows`, `statementErrors`, `idleTimeoutSeconds` and `maxLifetimeSeconds`. Sequence starts at 1;
the next request must send 2. A full final page can require an additional empty
fetch to discover EOF; the UI keeps the last non-empty page visible.

The opaque ID is randomly generated. The lease is bound to the username and
hash of the issuing bearer token. Another token or account cannot read or close
it. SQL text and bearer credentials are never placed in cursor URLs. Native
streams retain snapshot ownership; inserting data between pages does not add
new rows to the open cursor.

## Bounds and lifecycle

- The console's streaming and managed cursors share a quota: eight live cursors
  per process, at most two per admin owner. Native-engine resource limits apply
  in addition. Saturation returns 429; query admission can return 503.
- Managed cursor idle timeout: 30 seconds; absolute lifetime: 120 seconds.
  A background reaper checks every second, independent of incoming requests.
- Managed page size: 1–500 rows; native page buffer: 1 MiB encoded; managed
  response row budget: 2 MiB. Large individual records or projected pages can
  fail explicitly instead of silently omitting data.
- JWT expiry and current account privileges are checked on every page and each
  streaming batch. Long SKIP loops also recheck authorization. An already expired
  token cannot close its lease; idle/absolute expiry still reclaims resources.
- Requests use existing execution admission and cancellation. Managed cursors
  hold admission permits only while producing a page, not while idle. Streaming
  holds its permit until teardown. Cursor scans use a conservative large-work
  cost instead of consuming the protected small-query share; validation cost
  scales with input size. Per-request configured execution deadlines
  may be shorter than the 120-second cursor/stream ceiling (default 30 seconds).
- Page requests are serialized. Concurrent requests or an unexpected sequence
  return 409. A lost page is not replayed. Reconcile writes using their request
  key before reopening prepared results; reads may be rerun. No automatic query
  or write retries exist.
- Cursor state is process-local. Multi-worker deployments need sticky routing
  for open/next/close. Worker restart invalidates IDs; it does not silently open
  a different snapshot. The UI reports the failure and retains the request key
  for reconciliation. Share the durable journal across workers using the same
  database; a spool belonging to another worker is unavailable, never reexecuted.
- On ordered shutdown, active requests drain, idle cursor leases close, then
  document/graph engines shut down. Cursor close cancels the native execution
  scope before waiting for an in-flight page lock.

These are resource/lifecycle contracts, not a new large-scale capacity acceptance
or a guarantee of a universal hard RSS limit. Existing database/platform
limitations remain applicable.

## Validation

Local WSL validation on 2026-11-04: **322 Python tests passed, 5 skipped and
18 subtests passed**; **123 frontend tests passed**. Frontend test type checks,
ESLint, production build, entry bundle budget and real-Chromium smoke checks
pass. Full npm audit reports zero vulnerabilities. Built dashboard output is
updated at `NexoraDB/src/nexoradb/dashboard/static`.

- Native-backed API tests exercise diagnostics without SQL execution, source
  coordinates, trailing comments, snapshot stability, projection/filter/parameter
  semantics, LIMIT/SKIP, quotas, token binding, concurrent/replayed page requests,
  close, expiry, shutdown, stream completion and failure cleanup.
- Frontend tests cover split UTF-8/frame boundaries, large chunks with bounded
  callback batches, missing terminal markers, stream errors, abort, bounded
  retention, empty results, repeated Next clicks, stale cursor responses,
  debounced diagnostics and partial-script failures.
- Real Chromium tests cover the CodeMirror multiline editor, visible syntax
  colors, Ctrl+Enter, inline diagnostics, streaming retention and cursor next/close
  alongside the existing console smoke checks.

See [frontend testing](../frontend/TESTING.md). The release workflow includes the
new native-backed query workspace tests. No commit, staging, tag or publication
is performed by this development change.

Editor integration reference: [CodeMirror documentation](https://codemirror.net/docs/ref/).
