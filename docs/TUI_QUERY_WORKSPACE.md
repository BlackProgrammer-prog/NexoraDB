# Terminal query workspace

Run `nexoradb cli --url http://localhost:8000`, sign in, and open Query with F2.
The Textual workspace uses the same admin parser and query execution endpoints
as the React application. It does not implement another database executor or
silently fall back to Buffered when a requested mode is unavailable.

## Terminal editing

- Enter inserts a newline; F5 executes the complete document. Ctrl+Enter is an
  additional binding where the terminal transmits it distinctly. A terminal
  that sends ordinary Enter for Ctrl+Enter only inserts a newline.
- Tab indents in the query editor. Escape focuses the next control; Tab then moves
  between controls. Buttons provide alternatives to function keys.
- NexoraQL lexical highlighting covers strings, multiline comments, keywords,
  parameters and numbers without installing a tree-sitter SQL grammar. Server
  diagnostics, rather than highlighting, determine syntax/semantic validity.
- Validation is debounced by 400 ms. Superseded requests are cancelled and late
  responses cannot overwrite a newer document. Located errors are underlined;
  **Go to error** moves to the server's 1-based Unicode code-point location.
  Semantic errors without coordinates remain global messages.
- Parameters are a separate strict JSON object. NaN, Infinity, overflowed JSON
  numbers and scalar/array parameter roots are rejected. Data is bound through
  API parameters, never interpolated into query text.
- Query input is limited to 200,000 characters; parameter text to 65,536
  characters and its encoded data to 64 KiB. Oversized pastes are rejected in
  full, preserving the previous query. Terminal control sequences and bidi
  overrides are rejected in source; use escaped JSON parameters to supply such
  data deliberately. These editor limits do not replace server limits.
- F8/F9 or History buttons navigate at most 50 entries within a combined 1 MiB
  encoded budget, including parameters. Navigation preserves the unsent draft
  and never executes it. History and tokens are not written to disk.

## Execution and results

| Control | Behavior |
| --- | --- |
| Server cursor (default) | Opens a forward-only lease, 100 records per server page |
| F7 / Next cursor | Fetches the next sequence once; does not retry a lost page |
| Streaming preview | Consumes NDJSON incrementally with UI backpressure and a 120-second ceiling |
| Buffered | Uses the existing bounded execute endpoint; has no replay journal |
| F6 / Stop/close | Cancels active transport or closes the current lease |
| Preview arrows | Browse the retained data locally, 50 records at a time |
| Request status | Retrieves the current request key's durable execution status/receipts |

All modes retain at most 1,000 records within 8 MiB of encoded data, across all
statements together. Cells are literal text, limited to 1,000 characters and
50 columns per display page. Nested records and script receipts are readable
JSON. The preview keeps the most recent records; it is not a full export.
The encoded-data budgets do not measure Python object overhead or total RSS.
The log is bounded to 100 rendered lines and individual messages to 4,000
characters. The workspace scrolls vertically on small terminals.

Streaming updates are limited to approximately ten redraws per second plus the
final state, with at most 50 table rows mounted. The transport awaits its consumer
before reading the next batch. A batch ends at 128 records or approximately
1 MiB (a single legal frame can be up to 2 MiB). Missing terminal completion,
an explicit error frame, malformed UTF-8/JSON, contradictory frames and data
after completion fail the operation. Already received data remains a labelled
incomplete preview. Document fields such as `success: false` are ordinary data;
only protocol errors and `statementErrors` indicate execution failure.

SELECT/JOIN, multiple result sets, graph operations and mutation scripts inherit
the server contract described in [QUERY_WORKSPACE.md](QUERY_WORKSPACE.md),
including native snapshot ownership, lookup semantics, preflight restrictions,
transaction limits, admission and disk-spool quotas. A SELECT and its lookup
targets share a snapshot; different SELECT statements do not implicitly share
a script-wide transaction. Sorting/grouping syntax not implemented by the
server is not introduced by the terminal editor.

## Cancellation and reconciliation

Each explicit Streaming/Cursor run generates and displays an Idempotency-Key.
Side-effecting programs finish before delivery and use the server journal;
reopening the same key must not execute writes again. **Request status** inspects
the outcome without executing the query. Running again with F5 creates a new
key, so reconcile an uncertain write before doing so. Buffered execution retains
its existing behavior and does not offer this replay protection.

Stop cancels the HTTP task and closes known leases. Busy state prevents duplicate
runs and overlapping page fetches. The workspace tracks cleanup requests and
waits for them on logout/unmount before closing its HTTP client. If an open
response is lost before its cursor ID is known, or close cannot be acknowledged,
the server lease reaper is the fallback (30-second idle / 120-second lifetime).
Cancellation does not undo committed writes and is not proof that a mutation
never ran. There are no automatic write, query or cursor retries.

Query HTTP calls have a 35-second absolute deadline; close requests have three
seconds. Configured server deadlines may be shorter. HTTP redirects, environment
proxies and outbound cookies are disabled; non-JSON errors are not printed as
gateway HTML. Buffered responses are capped at 8 MiB, error bodies at 64 KiB,
and stream frames at 2 MiB. Output is passed to Rich as literal Text and terminal
control characters are suppressed. An HTTP 401 asks the host console to discard
the current session.

Cursor state and prepared-result files are process-local. Multi-worker services
need the same sticky-routing arrangement as the React client; changing terminal
mode does not make a lease portable between workers. Idle cursor expiry is
reported explicitly, not handled by opening a fresh snapshot automatically.

## Verification

`tests/test_tui_query_workspace.py` covers headless Textual input/navigation,
terminal widths, Unicode markers, bounds, cancellation/late responses, cursor
sequence errors, literal rendering, redirect/token safety and wire framing. The
native integration case exercises the actual admin parser, JOIN cursor, NDJSON
stream and write replay journal, and skips explicitly when Release is not built.
The tests are also exercised on Textual 0.86.3 and 8.2.8; supported dependencies
are constrained to Textual `<9`.

```bash
NexoraDB/.venv/bin/python -m pytest tests/test_tui_query_workspace.py -q
```
