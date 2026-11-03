# Frontend development tests

Run inside WSL with Node.js 24.15+ (validated with 24.17.0):

```bash
cd frontend
npm ci
npm test                 # watch affected tests while developing
npm run test:run         # single run, suitable for CI
npm run test:types       # type-check tests and their imported source
npm run lint
npm run build
npm run check:bundle
```

The suite uses Vitest, React Testing Library and jsdom, with React's development
runtime. Root StrictMode is explicitly enabled for effect-replay and session
tests. Fixtures use fake credentials, fake clocks and mocked transports; no real
database, API, browser profile or user session is accessed.

## Current matrix: 98 cases in 10 files

| File in `tests/` | Cases | Behavior covered |
| --- | ---: | --- |
| `httpClient.test.ts` | 23 | Bearer headers without cookies, unsafe paths/endpoints, 204, API errors, 401, old-session races, HTML/malformed JSON, UTF-8 byte limits, chunked responses, cancellation and timeout |
| `editorForms.test.tsx` | 19 | Invalid/scalar JSON in document/node/edge editors, valid empty objects, reopening forms, switching collections with the same document ID |
| `monitoring.test.tsx` | 11 | Burst coalescing and React commit count, sample cap, hidden tab pause/resume, connection caps, invalid timestamps, account changes and StrictMode cleanup |
| `rendering.test.tsx` | 10 | 100,000 input rows with only 50 mounted items, pagination boundaries, shrinking results, delayed/memoized JSON serialization, 2,500-row/100-column query results, escaped hostile text and errors |
| `useAsync.test.tsx` | 7 | Root StrictMode replay, reversed response order, changed resources, stale refetch callbacks, unmount cancellation, retry recovery and synchronous exceptions |
| `authTokenStorage.test.ts` | 7 | Memory-only credentials, no cookies/Web Storage persistence, legacy cleanup, expiry, replacing timers, unsubscribe, fresh-page state and blocked browser storage |
| `authProvider.test.tsx` | 7 | Root StrictMode session revalidation, valid sign-in, unsuitable accounts, rejected login, delayed responses after logout and external invalidation |
| `previewJson.test.ts` | 7 | Unicode, million-element sparse input, wide/deep objects, cycles, bigint, large strings and prototype-like keys |
| `queryRunner.test.tsx` | 4 | Success, replaced queries, errors clearing stale results and unmount cancellation |
| `documentFetching.test.tsx` | 3 | Controlled/empty parent results do not trigger duplicate fetching; standalone lists retain internal fetching |

Tests assert behavior and resource bounds instead of machine-specific elapsed-time
thresholds. In particular, the Profiler test verifies that 500 metrics events
produce one committed update at the next 250 ms flush, and DOM tests verify the
50-row limit. These are not FPS, browser-paint, layout or Core Web Vitals benchmarks.

## Browser integration checks

After building, run the existing real-Chromium production smoke suite from the
repository root:

```bash
NexoraDB/.venv/bin/python -m pip install playwright==1.64.1
NexoraDB/.venv/bin/python -m playwright install chromium
NexoraDB/.venv/bin/python frontend/scripts/browser-smoke.py
```

An existing Chromium can be selected with `NEXORADB_BROWSER_EXECUTABLE`.
The browser suite covers actual chunk loading, logos, canvas creation, CSP,
transport rejection, request cancellation and session navigation. It complements
the development component tests, which do not provide a layout/rendering engine.

The release workflow runs unit/component tests, their type check, lint, build and
the bundle budget. Browser integration checks currently run separately.

## Regressions found by the new tests

- JSON editors accepted truthy scalar JSON such as `42`, `true` and `"text"`.
  They now require an object before calling the save handler.
- A synchronous loader exception escaped the async hook's normal error state.
  Synchronous and asynchronous failures now follow the same recovery path.
- An out-of-range monitoring timestamp could throw during a React state updater.
  Timestamps now fall back to receipt time when outside JavaScript's date range;
  sample formatting happens before the state updater, and the formatter is reused.

The admin credential model is unchanged: tokens live in tab memory and are sent
as `Authorization: Bearer ...`; they are not stored in cookies. Reloading signs
the user out. An HttpOnly-cookie session would require coordinated server-side
session and CSRF changes and is not implemented by this test suite.

References: [Vitest environments](https://vitest.dev/guide/environment),
[React Testing Library setup](https://testing-library.com/docs/react-testing-library/setup/).
