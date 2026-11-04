# TUI performance and security review

This review covers the Textual console's login, monitoring, application tokens,
graph algorithms and query workspace. It is a code review with regression and
integration tests, not a production security certification or an FPS benchmark.

## Changes

| Area | Finding | Implemented behavior |
| --- | --- | --- |
| Monitoring rendering | Every message rebuilt the connection table and chart | One pending snapshot, at most four flushes per second, 200 displayed connections, keyed row updates, unchanged cards skipped |
| Inactive tabs | Monitoring continued rebuilding hidden widgets | Latest state is retained; rendering pauses outside the monitor tab |
| Traffic history | Message bursts changed sample timing | At most one sample per second, 60 samples maximum; gaps/stale data clear the chart |
| Monitoring transport | urllib fallback and unbounded synchronous WebSocket messages | Async Socket.IO over WebSocket, 256 KiB message ceiling, no binary attachments, TLS verification, no redirects, proxies or cookies |
| Connection failures | Health polling could appear to provide complete metrics | Fallback verifies `/auth/me` first, then reports HTTP reachability only; database health and resource metrics remain explicitly unavailable |
| Task ownership | Clicks could start concurrent login/register, token and algorithm requests | One task per operation group, with cancellation and cleanup owned by the screen |
| Logout/stale responses | Worker threads could update detached screens | Tasks are cancelled and awaited; closing/leaving screens reject new work and ignore late UI updates |
| Graph execution | Graph names and node IDs were interpolated into query text | Algorithm allowlist, NexoraQL identifier validation and bound parameters; statement failures cannot display as success |
| Terminal output | Rich markup/control sequences and unlimited logs | Literal Rich Text, control filtering, 200 log lines, 16 KiB JSON previews, explicit truncation |
| Credentials | Created tokens were printed into log history | One read-only, masked credential field; explicit 30-second reveal, auto-hide on tab changes, clear on logout/unmount |
| Clipboard | Password-masked Input can still copy its raw value in Textual | Masked password/token fields reject copy/cut; issued tokens become copyable only after Reveal |
| Token creation | No expiry was the default | Default expiry is 86,400 seconds; an explicitly empty field keeps the server's non-expiring option |
| Query editor | Lexical highlighting scanned the full document on every change | Edits coalesce for 80 ms; server diagnostics keep their independent 400 ms debounce |
| Preview clearing | Clearing during a stream reset the received count | The active operation retains its total while discarded row references are released |

All console HTTP calls use the bounded async transport. In-flight requests do
not retry automatically. Monitoring refresh is limited to once per second;
reconnection has one owner with exponential backoff capped at 30 seconds. The
monitoring snapshot parser rejects invalid types, non-finite numbers, negative
counters and out-of-range values before presentation.

Token and algorithm output never invokes Rich markup parsing. Names and error
messages are displayed as literal data. Token metadata is restricted to selected
non-secret fields; the credential itself is excluded from log output. The issued
credential field rejects editing and paste. Login passwords are cleared when
submitted and before asynchronous teardown. Python objects are released on exit;
this is not a guarantee of cryptographic memory zeroization.

Ctrl+C copies a supported focused selection; a masked password field copies
nothing. Ctrl+Q always quits. The root App routes this explicitly because older
Textual versions otherwise handle Ctrl+C as a priority Quit binding before the
child input. F6 remains the query Stop/close control.

The console continues to accept explicitly configured HTTP development URLs.
Remote deployments must use HTTPS/WSS with valid certificates. Certificate
verification is not disabled, and redirects cannot move a monitoring
authentication payload to another endpoint.
The WebSocket endpoint also retains the configured service URL's path prefix,
so a deployment under `/database/` connects to `/database/socket.io/` rather than
authenticating to a different Socket.IO application at the host root.

## Dependency audit

The project environment was audited with `pip-audit` from an isolated tool
environment, after upgrading compatible packages. Product metadata now requires
at least AnyIO 4.14.2, Certifi 2026.10.18, Cryptography 50.0.0 and aiohttp 3.14.5.
The build-system Setuptools minimum is 84.1.0. Development-environment HTTPX2,
HTTPCore2, urllib3 and pip were also upgraded; HTTPX2 is not used by the TUI's
`httpx` transport.

Relevant upstream references include the
[AnyIO TLS hostname advisory](https://github.com/agronholm/anyio/security/advisories/GHSA-82r6-8w77-94w6),
[Certifi trust-store update](https://github.com/certifi/python-certifi/security/advisories/GHSA-6q48-6c7w-9gw2),
[aiohttp client controls](https://docs.aiohttp.org/en/stable/client_reference.html)
and [Socket.IO asyncio clients](https://python-socketio.readthedocs.io/en/stable/client.html).

**One advisory remains in the development environment:** `dnspython 2.8.0`,
installed through optional PyMongo/BSON/benchmark tooling, is affected by
[GHSA-45c3-73f7-pv4m](https://github.com/rthalley/dnspython/security/advisories/GHSA-45c3-73f7-pv4m).
The patch is 2.9.0; pip rejects that version on this project's Python 3.10 because
it requires Python >=3.11. The TUI transports use standard resolver paths rather
than dnspython. This advisory is not suppressed, and the audit does not report a
clean environment. Resolving it requires a supported backport or a separate
Python/native-wheel ABI migration. Do not assume optional MongoDB SRV/DNS clients
are protected by the TUI changes.

The local report is `build/tui-security-audit/report.json`; its command returns
nonzero while the remaining advisory exists:

```bash
build/tui-security-audit/bin/python -m pip_audit \
  --path NexoraDB/.venv/lib/python3.10/site-packages --format json
```

The audit helper lives outside the product environment. The project environment
passes `pip check`; no dependency-advisory ignore list was introduced.

## Verification

The tests include a real local Socket.IO server, WebSocket redirect refusal,
oversized-message rejection, refused authentication, logout cleanup, terminal
injection, graph-query binding, repeated clicks, clipboard masking, hidden views,
burst coalescing, bounded traffic history and stable connection-row updates.

```bash
NexoraDB/.venv/bin/python -m pytest \
  tests/test_tui_query_workspace.py tests/test_tui_security.py -q
NexoraDB/.venv/bin/ruff check NexoraDB/src/nexoradb/cli
```

The same console tests are exercised with Textual 0.86.3 / Socket.IO 5.11.0 as
well as the installed versions. CI runs the security regressions; the native
release gate additionally exercises the actual database-backed query case.
