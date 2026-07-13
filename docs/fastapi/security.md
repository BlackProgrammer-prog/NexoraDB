markdown
# NexoraDB API Security

## App Token Authentication

### Token Format
nxapp_{header}.{payload}.{signature}


- **Header**: `{"alg":"HS256","typ":"NexoraDB-App-Token"}`
- **Payload**: `{"app_id":"...","scopes":[...],"iat":...,"exp":...}`
- **Signature**: HMAC-SHA256 signing

### Available Scopes
| Scope | Description |
|-------|-------------|
| query:execute | Execute NexoraQL queries |
| documents:read | Read documents |
| documents:write | Create/update documents |
| collections:read | Read collection metadata |
| collections:write | Create/drop collections |
| graphs:read | Read graph data |
| graphs:write | Modify graph |
| monitoring:read | Read system metrics |
| admin:apps | Manage app tokens |

### Creating a Token
```bash
curl -X POST http://localhost:8000/api/v1/apps/tokens \
  -H "Authorization: Bearer <admin-token>" \
  -d '{"appId":"my-app","scopes":["query:execute"]}'
### Using a Token
bash
curl -X POST http://localhost:8000/api/v1/query \
  -H "Authorization: Bearer nxapp_..." \
  -d '{"query":"SELECT * FROM users;"}'
### Production Security
API_TOKEN_SECRET must be at least 32 characters

DEBUG must be False in production

OpenAPI docs (/docs, /redoc) are disabled in production

All endpoints require authentication (except /health)

CORS origins must be restricted