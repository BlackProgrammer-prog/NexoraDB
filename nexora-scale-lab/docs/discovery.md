# Initial repository discovery

These are source observations, not a completed engine audit or runtime conformance
certification. No performance comparisons have been run.

## Architecture and integration

`CMakeLists.txt` builds a C++20 engine with optional pybind11 bindings and native
benchmarks. `core/DocEngine.h` uses RocksDB transactions. Python packaging lives
in `NexoraDB/pyproject.toml` and targets Python 3.10. The benchmark foundation uses
Python standard-library modules to fit the existing Python tools while keeping
installation and correctness checks independent of native builds.

`bindings/nexoradb_bind.cpp` exposes DocEngine, index definitions, insert_many,
insert_many_bulk, query operations, and explicit transaction methods. The precise
bulk acknowledgment and duplicate-key contracts still need inspection and live
tests before selecting an ingestion path.

`NexoraDB/src/nexoradb/api/driver.py` provides the real `NexoraDBClient` HTTP
driver: ping, execute, create_collection, insert_one, find and count. Execute
accepts typed parameters and returns rows and execution_time_ms. Its connection
pool is limited to 256 per client; a 500-client trial cannot simply request a
500-connection pool. Local embedded and remote HTTP measurements must be labeled
separately because their transport overhead differs.

## Observed operations

| Operation | Evidence | Remaining verification |
| --- | --- | --- |
| Create, insert, count | Public HTTP driver convenience methods | Real engine round trip and returned counts |
| Batch insertion | Native bindings; BATCH VALUES in query algorithm tests | Atomicity, acknowledgment, bounded request sizes |
| Point update/delete | `tests/test_query_contracts.py` routes `_id` operations to active transaction | Actual persistent-engine results |
| Index creation | Native `create_index` binding | Supported field types and equivalent plans |
| Transaction reads | Native transaction point-read binding | Isolation and conflict behavior |
| Transaction scans | Query contracts explicitly reject scans inside transactions | Report NOT_SUPPORTED on this query path |
| Aggregates, sorted scans, upsert | Not audited yet | Do not advertise support until verified |

The dataset logical `id` must map to NexoraDB `_id`, PostgreSQL primary key, and
MongoDB `_id`; adapters must normalize results before hashing or comparison.
Money is represented as integer cents to avoid cross-engine floating-point drift.

## Next implementation gates

1. Inspect storage durability defaults, native return contracts, parser grammar,
   query execution and indexing before implementing the NexoraDB adapter.
2. Build real isolated-engine integration tests; do not use mocked engines as
   evidence of database correctness.
3. Implement PostgreSQL and MongoDB adapters, explicit durability profiles and
   adapter conformance checks before any comparisons.
4. Add workload execution, bounded latency histograms, timeout/error accounting,
   state restoration, manifests and reports.

Resume is currently supported only as deterministic generator partition bounds;
there is no database load checkpoint or claim of safely resumable writes yet.
Age/country distributions are simple synthetic distributions, not calibrated
population models. Zipfian access, worker orchestration and workload generation
remain to be implemented.
