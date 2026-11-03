# Frontend performance and security review — 0.5.1

Reviewed locally in WSL on 2026-11-03. This covers the React console and the
server controls directly supporting it. It is not a penetration-test certificate
or an acceptance test of the database at industrial scale.

## Findings and changes

| Area | Finding | Implemented change |
| --- | --- | --- |
| Initial JavaScript | All route pages, Chart.js and Cytoscape were eagerly reachable | Route-level lazy imports; a shared lazy graph viewer; loading and recovery boundaries |
| Brand assets | `/brand/...` bypassed the `/admin/` deployment base | Both login and dashboard logos use `import.meta.env.BASE_URL` |
| Document fetching | Parent and child both fetched documents; sidebar repeated collection loading | Controlled document views consume parent results; sidebar receives parent collections |
| Request ownership | Refetches and route changes could retain requests or stale results | Abort signals, supersession guards, reset on resource changes; cancellation on unmount |
| Result rendering | Every row/card rendered at once; raw JSON serialized immediately | 50 rows/cards per page, 50 query columns, text preview limits and an on-demand bounded JSON preview |
| Graph layout | Force-directed layout ran synchronously for large graphs | Circle layout above 300 nodes; device pixel ratio capped at 2; explicit 1,000-node/10,000-edge viewer limits |
| Graph selection | An old node response could overwrite a newer selection | Previous selection fetch aborts; viewer fetch/selection abort on teardown |
| Live monitoring | Every incoming update could trigger a chart update | Coalesce updates to at most 4 per second, retain 30 samples, cap displayed connections at 500, pause painting/heartbeat while hidden |
| Browser credential persistence | Admin JWT persisted in localStorage | Memory-only token, legacy storage cleanup, expiry timer, invalidation on 401; reload requires sign-in |
| API transport | Production default pointed at localhost; redirects and unbounded responses were accepted | Same-origin production default; explicit remote URLs require HTTPS; no URL credentials; reject redirects and non-JSON responses |
| Browser resource limits | Large responses could allocate/render without a console budget | 35-second request timeout, 1 MiB request / 8 MiB streamed-response limits; bounded growable byte buffer |
| Browser policies | No console CSP or frame policy | CSP, frame denial, no-referrer, nosniff, permissions policy; HSTS on production HTTPS |
| HTTP caching | No distinction between hashed assets and sensitive responses | Successful content-hashed assets cache for a year; HTML, unversioned files and API responses use no-store |
| Server authorization | Admin endpoints generally verified JWT signatures without live account privileges | Require signed admin role AND current active admin account on protected admin HTTP routes |
| Monitoring authorization | Connection authentication did not continuously validate privileges | Verify active admin at connect, heartbeat and before sending metrics; disconnect expired, disabled, deleted or demoted sessions; no unauthenticated broadcast |

No automatic retry was added for mutations or queries: aborting a browser request
does not prove that a server-side write did not commit.

## Measured build and validation

- Entry JavaScript decreased from approximately **950 kB to 244 kB**, about **74%**.
  This compares emitted entry-file bytes, not an end-to-end latency or Core Web
  Vitals measurement. Route chunks are additional downloads when visited.
- The production graph viewer is a separate approximately 438 kB chunk. The
  monitoring/dashboard chunk is approximately 213 kB. The prior chunk warnings
  and ineffective mixed static/dynamic graph import warning are gone.
- A `300 KiB / 100 KiB gzip` entry budget is checked by `npm run check:bundle`.
  The release workflow now runs lint and this budget check.
- `npm run lint`, `npm run build`, lockfile portability and `npm audit` pass;
  audit reports zero vulnerabilities, including development dependencies.
- Full Python suite: **302 passed, 5 skipped, 18 subtests passed**. Initial review
  used the existing native binary; final release preparation rebuilt native and
  wheel as 0.5.1 and repeated validation. See the
  [candidate report](releases/validation-v0.5.1.md).
- Chromium exercises the actual production build with API fixtures: deferred
  chunks, logo image loads, no persistent auth token, 401/expiry/reload logout,
  one document fetch, cancellation of an obsolete collection request, modal
  state reset, 50-row rendering for 2,500 results, escaped XSS payloads, response
  size limits, redirect rejection, non-JSON rejection and graph canvas creation.
  The smoke server applies the same CSP middleware; no page/CSP errors occur.
- Backend regressions cover current-role/status enforcement, deleted users,
  monitoring expiry and privilege revalidation, and browser security policies.

## Reproduce in WSL

Development regression coverage now includes **98 passing tests in 10 files**;
see the [frontend test guide](../frontend/TESTING.md) for the case matrix and watch
commands. These tests found and fixed scalar-JSON editor validation, synchronous
loader error handling and out-of-range monitoring timestamp crashes. Development
tests also check render/serialization bounds and StrictMode cleanup, alongside
the production-browser checks described below.

From the repository root, with Node.js 24 on PATH:

```bash
cd frontend
npm ci
npm run lint
npm run build
npm run check:bundle
node scripts/check-lockfile.mjs
npm audit
cd ..
NEXORADB_TEST_NATIVE_PATH="$PWD/NexoraDB/src/nexoradb/native/nexoradb.cpython-310-x86_64-linux-gnu.so" \
  NexoraDB/.venv/bin/python -m pytest tests -q
```

Browser checks use optional tooling installed in the WSL test environment:

```bash
NexoraDB/.venv/bin/python -m pip install playwright==1.64.1
NexoraDB/.venv/bin/python -m playwright install chromium
NexoraDB/.venv/bin/python frontend/scripts/browser-smoke.py
```

Set `NEXORADB_BROWSER_EXECUTABLE` to use an existing Chromium executable instead.
The browser checks use fixtures and never issue queries against a real database.
Build output is `NexoraDB/src/nexoradb/dashboard/static` and is included by the
existing wheel package-data configuration.

## Deployment requirements and limits

- **Document pagination currently bounds the DOM, not the legacy admin endpoint.**
  That endpoint still materializes the collection server-side. The console now
  refuses responses over 8 MiB instead of rendering them. Use a constrained query
  or the existing bounded cursor/streaming API for large data; server-side admin
  pagination remains necessary for browsing very large collections in this UI.
- Use HTTPS and `NEXORADB_ENV=production`, independent strong authentication/API
  secrets, explicit HTTPS `NEXORADB_ADMIN_ORIGINS`, and a trusted TLS proxy setup.
  Provision the root account before exposing the first-run registration endpoint.
- Deploy login rate limiting and access restrictions at the gateway. Existing
  process admission is not a per-client brute-force limiter. MFA and a distributed
  session/revocation service are not supplied by this frontend change.
- Local logout erases the browser token; it does not revoke a copied JWT while
  the account remains active. Consider a short `NEXORADB_ACCESS_TOKEN_TTL_SECONDS`
  and a server-side revocation/session design for stricter requirements. Account
  deletion, disablement and role changes are now checked server-side.
- Memory-only storage reduces credential persistence, but JavaScript running in
  a compromised page can still access a live token. A persistent HttpOnly-cookie
  or BFF session would require a coordinated authentication/CSRF redesign.
- Application tokens remain encrypted at rest and retrievable by an authorized
  administrator under the existing API contract. This is not a one-time-secret
  registry; the UI now describes that behavior accurately. Restrict admin access
  and token scopes and rotate exposed credentials.
- The built-in CSP permits inline **styles** for canvas/layout libraries; scripts
  remain restricted to self. Allowed connection origins come from validated
  server origin settings. With a separate static host/API, reproduce these
  headers and explicitly authorize the HTTPS API and WSS monitoring origin there.
- UI budgets and the graph layout threshold are explicit defaults, not evidence
  of a measured universal hardware limit. No real-user CWV, mobile-device,
  multi-browser or high-concurrency load certification is claimed.

References: [React lazy loading](https://react.dev/reference/react/lazy),
[React effect race conditions](https://react.dev/reference/react/useEffect),
[OWASP browser storage guidance](https://cheatsheetseries.owasp.org/cheatsheets/HTML5_Security_Cheat_Sheet.html).
