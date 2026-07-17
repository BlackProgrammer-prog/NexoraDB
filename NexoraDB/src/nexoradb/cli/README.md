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

## Static admin bundle

The backend also serves `src/nexoradb/dashboard/static` at:

```text
http://localhost:8000/admin
```

Use `nexoradb dashboard --mode static` if you want to serve that folder directly on port `5173`.
