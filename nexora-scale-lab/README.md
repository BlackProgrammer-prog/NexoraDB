# Nexora Scale Lab

Dedicated project directory for the 100,000,000-record comparison of NexoraDB,
PostgreSQL, and MongoDB described in the supplied task.

This directory is separate from the existing `benchmarks/` directory.
The implementation provides deterministic streaming generation, three real
database adapters, a small smoke runner, and a reproducible comparison protocol
for YCSB-inspired A/B/C plus indexed equality lookup. The protocol implements
randomized repetitions, conditioning, full integrity checks, exact write replay,
restoration, resource sampling, checkpoints, and JSON/CSV/Markdown/HTML reports.

See [the complete methodology and execution commands](docs/methodology.md).
For an exploratory 100M profile with 42 minutes of scheduled workload phases,
use `bash scripts/run_compact.sh --plan` first. Loading and verification add
time; there is now no overall or per-job time cap. This profile uses sampled
integrity checks and one repetition. Its live terminal dashboard shows the
engine, stage, record count, rate, ETA, and verified-trial count while saving logs.
Read-only planning (does not start any trial):

```bash
bash scripts/run_publication.sh --plan
```

The full benchmark must only run after explicit authorization.

## Run the foundation

Python 3.10 or newer; no third-party dependencies for these commands:

```bash
cd nexora-scale-lab
python3 -m scale_lab plan --records 100000000
python3 -m scale_lab dataset --records 1000 --batch-size 128
python3 -m unittest discover -s tests -v
```

`dataset` generates a streaming checksum summary without storing records.
Requests of 100M records or more require `--allow-large` even for generation.
`plan` only estimates resources and never generates records or opens a database.
The default dataset target is exactly 100,000,000; smoke sizes are explicit.

Data uses integer cents for balances, UTC timestamp strings, and fixed-width
string primary keys. Generation depends only on seed and record ID, allowing
independent worker partitions and restart without replaying an RNG stream.
SHA-256 fingerprints cover canonical JSON records in ascending ID order.
Partition digests must not be combined as though they were a full-stream digest.

See `docs/discovery.md` for repository findings and remaining verification.

## Database smoke trials

Run from this directory. The native adapter uses the compiled extension from
the repository root. The extension must expose DocumentDurability.Durable.

```bash
export PYTHONPATH="$(pwd)/.."
python3 -m scale_lab prepare --records 1000 --namespace scale_lab_smoke
python3 -m scale_lab load --records 1000 --namespace scale_lab_smoke
python3 -m scale_lab run --records 1000 --namespace scale_lab_smoke \
  --workload ycsb-c --clients 10 --operations 10000 --output results/native-smoke.json
SCALE_LAB_TEST_NATIVE=1 python3 -m unittest discover -s tests -v
```

Each experiment requires a fresh namespace. `prepare` refuses existing
namespaces. `load` refuses nonempty ones and validates counts and sampled
logical records. Interrupted loads are not resumed or retried automatically.
`run` validates the baseline before measurement. A/B mutate status fields;
prepare and load a fresh namespace before the next trial. Run does not perform
automatic state restoration. Only C leaves the dataset unchanged.

Install optional drivers with `python3 -m pip install -r requirements.txt` in
your own virtual environment. Configure `SCALE_LAB_POSTGRES_DSN` or
`SCALE_LAB_MONGO_URI` and select `--database postgresql` or `--database mongodb`
for each of the same prepare/load/run commands. Integration tests use those
variables and create/drop uniquely named test namespaces.

`docker/compose.yaml` provides standalone PostgreSQL and MongoDB:

```bash
export SCALE_LAB_POSTGRES_PASSWORD='choose-a-local-test-password'
docker compose -f docker/compose.yaml up -d --wait
export SCALE_LAB_POSTGRES_DSN='postgresql://scale_lab:choose-a-local-test-password@localhost:55432/scale_lab'
export SCALE_LAB_MONGO_URI='mongodb://localhost:57017/scale_lab'
```

Durability requests: native RocksDB Durable, PostgreSQL synchronous_commit=on
with fsync checked, and MongoDB w=1,j=true on a standalone server. These do not
claim replication equivalence. PostgreSQL stores JSONB documents plus a text
primary key; MongoDB/NexoraDB add `_id`, removed during result normalization.
All prepare commands create a username secondary index before loading.
MongoDB ordered batches may partially commit on failure; NexoraDB/PostgreSQL
batches are atomic. Ingestion includes generation and serialization.

Latency uses a bounded logarithmic histogram (at most 1% bucket error), monotonic
nanosecond timing and includes failed attempts. Read/write operation mixes are
seeded probabilistic mixes, so measured counts can differ from the target ratio.
JSON outputs explicitly mark these smoke trials as ineligible for publication.
The new comparison protocol adds open-loop load, resource metrics, warm-up,
repetitions, restoration and CSV/HTML reports. YCSB D/E/F, range, aggregate and
join workloads remain outside its implemented and declared comparison scope.

Native integration is exercised against the real extension. PostgreSQL and
MongoDB integration tests skip unless their connection variables are configured;
driver implementation alone does not certify those adapters against live servers.

## Locally installed WSL services

The local installation uses Ubuntu's PostgreSQL 14 package on loopback port
55432 and the MongoDB 8.0 official repository on loopback port 57017. MongoDB
authorization is enabled, its WiredTiger cache is capped at 4 GiB, and the
benchmark principal has readWrite on scale_lab. PostgreSQL uses a dedicated
scale_lab login/database, password authentication over TCP and fsync=on.
Credentials are in the ignored, mode-600 `.env.local`; do not commit that file.
MongoDB also has a `scale_lab_admin` user administrator in `admin`, created
through the [official localhost exception](https://www.mongodb.com/docs/v8.0/core/localhost-exception/).
It uses the same temporary password; benchmark connections use the restricted
`scale_lab` principal, not the administrator.
The Docker configuration above is an alternative, not the local installation.

After a WSL restart, start the services from Windows PowerShell:

```powershell
wsl -d Ubuntu -u root -- bash /home/parham/NexoraDB/nexora-scale-lab/scripts/system/start-local-services.sh
```

Inside Ubuntu WSL, run all adapter tests:

```bash
cd /home/parham/NexoraDB/nexora-scale-lab
source .env.local
export PYTHONPATH=/home/parham/NexoraDB
SCALE_LAB_TEST_NATIVE=1 .venv/bin/python -m unittest discover -s tests -v
bash scripts/check_local_cli.sh
```

Run a **full 100M-record dataset with preliminary read workload** only when ready:

```bash
cd /home/parham/NexoraDB/nexora-scale-lab
bash scripts/run_100m_smoke.sh --confirm-100m all
```

Replace `all` with `nexoradb`, `postgresql` or `mongodb` for one engine. This loads
exactly 100,000,000 records per selected engine, sequentially, then runs 100,000
uniform point reads with 10 clients. Override `SCALE_LAB_CLIENTS` and
`SCALE_LAB_OPERATIONS` to change those workload parameters. Logical JSON size
is estimated from 1,000 records; disk budget uses a 4x per-engine planning
heuristic and is not a guarantee. Results/data are retained, loads are not
resumable, and every new invocation uses a new namespace consuming more space.
Loading progress is printed every five seconds. Each result is written to a new
timestamped results directory. This script does not implement the full
warm-up/repetition/resource-control protocol required for published comparisons.
