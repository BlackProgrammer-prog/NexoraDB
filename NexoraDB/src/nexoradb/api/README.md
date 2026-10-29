# NexoraDB External API and Python Driver

## Write durability

Document writes now default to **Durable** in native and service startup. Select
`NEXORADB_ROCKS_DURABILITY=durable|buffered` explicitly for the server; hardware tuning cannot override it.
Inspect `SYSTEM INFO;`, `SHOW PERFORMANCE;` or native `durability_report()` for effective flags.
Buffered keeps WAL enabled but does not promise machine/power-loss durability. Transaction staging is not commit;
HTTP 200 alone is not a successful write ACK when `statementErrors` is nonempty. IO/commit failure or a lost
write response can have an unknown outcome. See `docs/cpp/DOCUMENT_DURABILITY_CONTRACT_FA.md` in the source tree
for migration, graph scope, crash evidence and retry limitations.

## Server resource policy

Admin/API startup now defaults to `NEXORADB_ROCKS_AUTO_TUNE=true`: it bounds RocksDB workers using
visible CPU affinity/cgroup v2 quotas and conservatively caps the configured cache+memtable budget.
It never increases `NEXORADB_ROCKS_MEMORY_MB`; disable this startup policy with
`NEXORADB_ROCKS_AUTO_TUNE=false` for fixed-budget experiments. This is not a whole-process RSS limit
and does not change durability. See `docs/cpp/HARDWARE_AWARE_TUNING_FA.md` in the source repository
for scope, read-only inspection and unsupported environments.

## Issue an application token

Create a token from the admin API with an admin bearer token:

```bash
curl -X POST http://localhost:8000/apps/tokens \
  -H "Authorization: Bearer <admin-token>" \
  -H "Content-Type: application/json" \
  -d '{"appId":"billing-service","appName":"Billing Service","scopes":["query:execute"]}'
```

## Use the Python driver

```python
from nexoradb.api import connect

db = connect(url="http://localhost:8000", token="<app-token>")

result = db.execute("SHOW COLLECTIONS;")
print(result.rows)
print(result.execution_time_ms)
```

All driver traffic uses `/api/v1/query` and is authenticated with the app token.
The admin monitoring dashboard reports these clients as `api-driver` connections.
