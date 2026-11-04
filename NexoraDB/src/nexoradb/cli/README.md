# NexoraDB CLI and terminal commands

## Unified command

```bash
nexoradb run
nexoradb server
nexoradb dashboard
nexoradb cli
nexoradb dev
```

## Individual entry points

```bash
nexoradb-server
nexoradb-dashboard
nexoradb-cli
```

## Typical developer workflow

1. Start backend and React dashboard together:

```bash
nexoradb dev
```

2. Or run them separately:

```bash
nexoradb run
nexoradb dashboard
```

Run the backend as a background service that survives closing the terminal:

```bash
nexoradb run --service
nexoradb run --log
nexoradb run --log 100
nexoradb run --stop
```

3. Launch the Textual admin console:

```bash
nexoradb cli
```

Open **Query (F2)** for the integrated NexoraQL workspace: multiline editing,
lexical highlighting, server diagnostics, JSON parameters, Streaming and server
cursors. **F5** runs, **F6** stops/closes, **F7** requests the next server page,
and **F8/F9** navigate bounded in-memory history. Enter inserts a newline;
Escape focuses controls. Buttons cover terminals without these key sequences.

The default Cursor mode uses the same execution and replay-protection contract
as the React workspace. Record the displayed request key and inspect **Request
status** before retrying uncertain writes. The retained preview is capped at
1,000 records / 8 MiB, rendered 50 records at a time. See
[the terminal workspace contract](../../../../docs/TUI_QUERY_WORKSPACE.md).

The console also bounds/coalesces monitoring updates, owns asynchronous work per
screen, and displays server data literally. New app tokens default to a one-day
expiry and remain masked until **Reveal token (30s)**. Masked credentials cannot
be copied; switching tabs hides a revealed token. **Ctrl+Q** quits the console.
See the [performance/security review](../../../../docs/TUI_PERFORMANCE_SECURITY.md)
for transport limits, dependency-audit results and remaining limitations.

## Static admin bundle

The backend also serves `src/nexoradb/dashboard/static` at:

```text
http://localhost:8000/admin
```

Use `nexoradb dashboard --mode static` if you want to serve that folder directly on port `5173`.
