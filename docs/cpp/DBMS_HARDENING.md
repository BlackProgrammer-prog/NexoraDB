# DBMS hardening: implementation status and contracts

This is an incremental implementation, not a production-readiness certificate.
Existing roadmap items remain independent; this document does not mark them done.

## Required product decisions

- Keep `RENDER GRAPH` unchanged. No Cypher dependency or syntax is introduced.
- Keep document storage as the source of truth. Graphs are derived projections.
- Startup must not report readiness until required graph projections are verified
  against recovered document state and repaired, or rebuilt when repair is unsafe.
- Fail closed on recovery errors. Never publish a partial rebuild as healthy.

## Changes in this increment

- Application queries are parsed once, then every statement is checked against
  operation scopes before any statement runs. Unknown/maintenance/job statements
  are denied to application tokens until their permissions and ownership are defined.
- `query:execute` alone no longer authorizes reads, writes, or DDL. Add the appropriate
  `documents:read`, `documents:write`, `collections:read`, `collections:write`,
  `graphs:read`, `graphs:write`, or `monitoring:read` scope when issuing tokens.
  These scopes are global operation permissions, NOT resource-level RBAC or tenancy.
- HTTP transactions must begin and finish in the same request; preflight rejects
  incomplete or nested transaction blocks. Maximum statements per request: 256.
- Document-only transactions route point updates, point deletes, point reads, and
  batch inserts through the active transaction. Statement failures stop the script
  and roll back an active transaction. Earlier autocommitted statements are not undone.
- Transactional scans, bulk update/delete, DDL, and graph operations are explicitly
  unsupported. Transactional writes with an attached GraphManager are temporarily
  rejected until durable projection integration exists. This is a deliberate safety
  restriction, not an implementation of cross-model transactions.
- Known development signing secrets are rejected in production, even if explicitly
  supplied in environment variables. Unknown environment names are rejected.
- Nested Python projection preserves object/array shape; EQ/NEQ SQL aliases are accepted.
- C++ numeric predicate comparison avoids int64-to-double rounding errors; string
  `"null"` is distinct from null in index predicate encoding.
- Dirty-graph startup no longer calls build while holding the registry mutex.
  Handle/build failures no longer silently allow startup success on that path.
- Collection scans throw on missing/reserved collections, decode failures, and
  iterator errors. Graph build catches scan errors and checks metadata write status.
  Existing callers of IterateCollection must now handle failures rather than relying
  on a silently truncated scan. These changes do NOT establish crash-safe graph recovery.

## Recovery design — NOT implemented yet

1. Recover RocksDB WAL first. The recovered document state is authoritative.
2. Commit document/index/counter mutations and a durable change event atomically,
   across every native, HTTP, Python, bulk and explicit transaction write path.
3. Store durable graph definitions, mapping schema/version, database identity,
   source sequence, applied sequence and generation identity in a versioned catalog.
4. Compare each projection checkpoint with the recovered source sequence. Reconcile
   updates, deletes and UNWIND edge changes by source document identity and revision.
5. Replay only a complete retained event interval, with idempotent application.
   Detect source rollback, catalog mismatch, missing intervals and graph corruption.
6. Otherwise rebuild from a consistent document snapshot into a NEW generation.
   Do not overwrite the only usable graph while rebuilding.
7. Persist graph data, checkpoint and manifest with checked durability barriers.
   Publish a complete generation atomically; directory durability is part of publish.
8. Open the readiness gate only after every required projection is verified.
   Static graphs need an explicit frozen-snapshot versus current-projection policy.
9. Retain the previous generation until publication/recovery is proven complete.

Graph WAL alone cannot detect a document commit that crashed before the graph
callback ran. A source-side transactional event or equivalent durable revision
contract is essential. File timestamps and node counts are not sufficient evidence
of consistency. `sync=false` commits also do not promise survival of power loss.

## Remaining engineering work

Follow-up source changes and compatibility notes are described in
[NEXORAQL_CONTRACT.md](NEXORAQL_CONTRACT.md). Implemented code is not equivalent to
native acceptance: C++ changes have only been checked with `-fsyntax-only` so far.

- [ ] Durable graph/source recovery protocol above, with startup readiness gating.
- [ ] Crash injection at every event/checkpoint/generation publication boundary.
- [ ] Native integration tests for all transaction routes and read-your-writes.
- [ ] Transactional scans/bulk operations and safe graph projection on commit.
- [ ] Resource-level authorization, tenant isolation, job/session/cursor ownership.
- [x] Typed query parameters through parser, HTTP, session and driver; input/AST budgets.
- [ ] Hard parse-time and execution deadlines with cooperative cancellation.
- [ ] Regex runtime/resource safety and exception-boundary audit.
- [ ] Unified numeric semantics across index scans, sorting and unique constraints.
- [ ] Native acceptance for implemented schema types, top-level strict mode and insert defaults.
- [ ] Schema evolution validation and recursive strict schema semantics.
- [ ] Native acceptance for v2 target JOIN lookup and typed point-read NotFound errors.
- [ ] Full JOIN cardinality/snapshot semantics and comprehensive typed storage errors.
- [ ] Graph filter/property mapping, object UNWIND and incremental relationship diff.
- [ ] Core projection pushdown, bounded streaming and nonduplicated driver results.
- [ ] Catalog IDs, dependency tracking, statistics and plan-cache invalidation.
- [ ] SQL-like aggregation, graph patterns and formally documented path semantics.
- [ ] Retry/idempotency protocol and explicit ambiguous-commit errors.
- [ ] Change-stream consumer API, TTL and multikey indexes.
- [ ] Secure deployment/TLS, key rotation, audit redaction and authentication throttling.
- [ ] Artifact provenance/SBOM and native wheel contract tests.
- [ ] Complete existing backup, WAL, scheduling, observability and CI roadmap.
- [ ] Replication/fencing/failover, then sharding only after single-node correctness.

## Verification

Python contract tests use fakes; they do not establish native durability or isolation.
Run from tests/ to avoid the root-level native module shadowing the Python package:

```bash
cd /home/parham/NexoraDB/tests
PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=../NexoraDB/src \
  ../NexoraDB/.venv/bin/python -m pytest \
  test_query_contracts.py test_parser_queries.py test_admin_auth_api.py \
  test_parser_algorithms.py test_query_algorithms_e2e.py -q -p no:cacheprovider
```

C++ changes require a fresh build and Debug/ASan/UBSan runs before acceptance.
No C++ build was performed in this increment. In particular, passing old binaries
does not verify these changes. The startup recovery acceptance criterion is OPEN.

Follow-up verification: the full collected Python suite passed (73 tests plus 18
subtests). Changed native translation units and new test code passed syntax-only
checking using the existing Debug compilation database. No object files or linked
binaries were generated. Reproduce the static check with:

```bash
python3 scripts/check_cpp_syntax.py \
  core/DocEngine.cpp core/IndexCodec.cpp query/CompiledQuery.cpp \
  query/CompiledUpdate.cpp query/DocumentView.cpp query/Evaluator.cpp \
  graph/GraphManager.cpp bindings/nexoradb_bind.cpp \
  tests/cpp/test_doc_engine.cpp tests/cpp/test_graph.cpp
```
