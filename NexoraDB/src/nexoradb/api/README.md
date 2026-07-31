# NexoraDB External API and Python Driver

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
