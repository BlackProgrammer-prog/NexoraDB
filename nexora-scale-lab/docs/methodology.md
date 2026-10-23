# Reproducible local-deployment comparison protocol

## Compact 100M profile with live progress and no time cap

`config/compact-100m.json` preserves exactly 100,000,000 requested records per
engine and uses all three engines, A/B/C plus indexed lookup, clients 1/10/50,
one repetition, indexes before loading, 10-second conditioning and 60-second
measurement. Its 36 trials require 42 minutes of scheduled workload time.
Count verification remains exact; field/integrity checks sample 1,000 keys plus
boundary keys instead of scanning every stored record after every phase. Updates
still have exact schedule replay and all replayed mutated keys are restored.
These are exploratory results, not the full publication protocol; unsampled
records have not been certified and no repeated-trial confidence interval exists.
The full profile below remains unchanged and uses full validation by default.

Loading, index preparation, count queries, replay and restoration take additional
unknown time. Completing the whole comparison within an hour **from empty
databases is not guaranteed**. `scripts/run_compact.sh` has no outer timeout and
this profile sets `job_timeout_seconds` to null, disabling its per-job watchdog.
It continues until completion, an actual failure, or Ctrl+C. Individual driver
timeouts and finite warm-up/measurement windows remain as configured. It never
substitutes fewer records. On Ctrl+C, artifacts and incomplete data are retained
and the CLI exits with code 130 without an alarming KeyboardInterrupt traceback.

Read-only plan inside WSL:

```bash
cd /home/parham/NexoraDB/nexora-scale-lab
bash scripts/run_compact.sh --plan
```

Only when you choose to execute, from Windows PowerShell:

```powershell
wsl -d Ubuntu -u root -- bash /home/parham/NexoraDB/nexora-scale-lab/scripts/run_compact.sh --confirm-100m
```

In an interactive terminal the live dashboard refreshes in place every second.
It shows engine/job, exact named stage, workload/client count, completed verified
trials, a stage progress bar, acknowledged loaded records, recent/average speed,
elapsed time and estimated remaining stage time. ETA is based on observed stage
throughput, not a promise of total run duration. Index creation, statistics
analysis, and COUNT calls expose no engine progress API here: their percentage
and ETA remain unknown rather than fabricated. A timestamp shows how recently
the counter changed; a slow/blocking database request is not falsely declared
stalled. A timed phase that is draining requests is labeled accordingly.

The per-stage units are records for loading/full validation/restoration, sampled
keys for sample validation, operations for offline replay, and seconds for timed
warm-up/measurement. Overall jobs/trials use completion counts, not a guessed
time-weighted total percentage. No terminal-control sequences are emitted when
output is redirected; plain JSON progress continues instead.

Each results directory retains `events.jsonl` (stage transitions/periodic events),
`live.json` (latest dashboard snapshot), and the original detailed
`job-*/attempt-*/stdout.jsonl` / `stderr.log`. Worker `progress.json` stores stage
counts/rate/ETA; measured `cell-*/in-progress.json` retains periodic raw counters.
Dashboard writes and its one-second status pulse add client overhead, included
in the current source snapshot and achieved throughput; they do not run database
queries for progress. Existing completed results and interrupted stores are not
deleted by launching a new experiment. This changed source/config cannot be
silently resumed into an older hashed study; the default command creates a new
results directory and starts fresh jobs.

This protocol defines a publishable **scope of comparison**, not an automatic
endorsement of future numbers. It measures the complete local deployment for
NexoraDB embedded, PostgreSQL TCP, and MongoDB TCP. It cannot support the claim
that one storage engine is intrinsically fastest: transport and driver work are
part of the comparison. There is no overall-winner calculation.

## Exact primary design

`config/publication.json` specifies exactly 100,000,000 generated records, seed
2026, 1,000-record batches, five independent repetition jobs per engine/index
policy, read-only cache conditioning for 60 seconds and measurement for 300
seconds. Concurrency is 1, 10, 50, 100, 250, and 500. Engines are randomized within
each repetition; index-policy order is randomized per engine; workload/concurrency
cell order is randomized but paired across engines within each repetition.
Experiments execute sequentially under a cross-output-directory advisory lock.

There are 30 independent load jobs and 720 measured workload trials. This means
3,000,000,000 records inserted over the entire experiment, not a 3B-record dataset.
The configured measurement/conditioning time alone is 72 hours. Full validation,
SQLite replay, restoration, index builds and loading add potentially much more.
Per-job watchdog defaults to 14 days and terminates the isolated worker process
group on expiration; it is not a per-request deadline. Nothing in the test suite
or `--plan` launches the large experiment.

Two separately reported ingestion policies retain the primary key throughout:

1. Username secondary index exists before loading.
2. Load first, then build the username secondary index.

Index time and ingest time remain separate. Ingest time includes generation,
serialization, native-driver/client transfer, durable acknowledgment, checksum
generation and the resource sampler. It is an end-to-end loading measurement.
PostgreSQL is ANALYZEd after loading. Actual username query plans are archived;
the primary profile refuses to label a query indexed unless its optimizer uses
a secondary index. Small smoke tables can legitimately choose sequential scans.

## Supported workload definitions

| Name | Operation mix | Returned logical work |
| --- | --- | --- |
| ycsb-a | Seeded probabilistic 50% point read / 50% update | Full document / one changed status field |
| ycsb-b | Seeded probabilistic 95% point read / 5% update | Full document / one changed status field |
| ycsb-c | 100% point read | Full document |
| indexed-read | 100% username equality lookup | Exactly one full document; limit 2 detects duplicates |

These are **YCSB-inspired**, not certified YCSB implementations. Workers own
disjoint interleaved primary-key partitions. This gives an exact deterministic
write-state oracle; it deliberately excludes cross-client key contention. The
default distribution is uniform within each partition. The optional `zipfian`
mode uses a discrete rejection sampler with configurable theta in (0,1), and
scrambles ranks with a deterministic affine bijection. `working_sets` lists
fractions of the unchanged 100M dataset; these are not smaller primary datasets.
Smaller record counts are explicitly labeled development/scaling experiments.

Every update writes a new `bench:seed:worker:sequence` status, so MongoDB cannot
silently turn repeated identical writes into no-ops while another engine rewrites
data. Attempted and achieved operation mixes are recorded, not assumed exact.
Each worker has the same access-sequence prefix across engines. A time-based
experiment naturally completes different prefix lengths on different engines.

## State and correctness

Each repetition/engine/index-policy job creates a fresh isolated namespace and
NexoraDB directory. Dataset fields use integer cents, canonical UTC timestamp
strings, arrays, objects and an explicit null. Logical `id` maps to `_id` on the
document engines; the extra `_id` is removed before comparison. PostgreSQL stores
the logical record as JSONB plus a text primary key. This is a document-model
comparison, not a typed relational schema comparison.

Loading builds an order-independent SHA-256 sum/xor fingerprint. Full validation
streams every record, verifies every field against the deterministic generator,
checks canonical key range and count, and compares the baseline fingerprint.
Unique database primary keys plus exactly N valid IDs prove complete coverage;
there is no giant in-memory ID set. This fingerprint is explicitly different from
the ordered `dataset` command's SHA-256 stream digest.

Before each measured workload, read-only conditioning runs on the configured
working set. A/B use point-read conditioning, indexed-read uses username lookup.
Warm-up preserves the exact baseline logical dataset. This is **warm-conditioned**,
not a verified cold cache, not an entirely RAM-resident dataset and not a claim
that write-specific paths have had an equivalent write warm-up.

Every measured read checks all returned fields. After measurement, each worker's
successful access schedule is replayed offline into a disk-backed SQLite table
of final expected status values. Disjoint key ownership removes cross-thread
write-order ambiguity. Then the whole database is streamed against this exact
oracle. Each changed key is restored, followed by a second full baseline scan.
The logical baseline is therefore equivalent before subsequent workloads.
Physical pages/fragmentation are not restored between cells; paired randomized
cell order and independent fresh loads per repetition address ordering effects,
not a claim of identical physical layouts.

An error or timeout can mean a write committed without acknowledgment. Such a
trial is preserved and cannot be certified, replayed as success, or silently
retried. The job stops. Failed/unverified attempts remain in an audit ledger and
prevent protocol passage even if a later resumed attempt succeeds. No failed
requests are dropped from their raw latency/counter statistics.

## Timing, offered load and metrics

Connections are prepared before the measured start barrier. Resource sampling
starts once all workers are ready; they retain connections until the final sample,
then close them outside the timing window. Warm-up and measurement are separate.
Monotonic high-resolution clocks record driver latency and end-to-end response
latency. Client result-checking CPU time is separately accumulated; its work is
still part of achieved throughput. Latency histograms are bounded logarithmic
histograms with <=1% bucket error, storing raw counts/sums/maxima, not invented
per-request samples. Quantiles are labeled approximate upper bucket bounds.

Closed-loop clients issue the next request only after completing the previous
one. No coordinated-omission correction is claimed. Optional open-loop mode
requires `target_rate`; constant-rate arrival slots are assigned across workers,
response latency includes scheduling lag, and slots whose lag exceeds
`max_queue_delay_seconds` are recorded as attempted/failed/timed-out/rejected.
This is an explicit bounded-queue shedding policy; overloaded arrivals do not
vanish. Unknown write outcomes invalidate correctness certification. Overload
studies with failures remain diagnostic artifacts rather than healthy-throughput
publication candidates under this strict profile.

Linux `/proc` supplies server process-tree CPU/IO, RSS, proportional set size,
sampled peaks, and separate generator CPU/IO/PSS. For embedded NexoraDB the server
and generator share a process, so they cannot be separated; total deployment
comparisons count that process only once. PSS is summed at each sample before
taking the joint peak. Summed PostgreSQL RSS double-counts shared pages and is
not the comparative memory metric. Sampled peaks can miss between-sample spikes;
short-lived processes can escape sampling. Sampler CPU overhead is recorded.
Kernel IO counters reflect the observation window, not all future background
compaction/writeback. Per-engine network bytes and common per-request server-side
execution time are unavailable and are not fabricated.

Server and generator CPU affinity must match. Engines share the same unrestricted
local WSL machine; vendor cache settings and versions are archived, not claimed
equal memory configurations. No cgroup RAM isolation or verified absence of
external host load is claimed. Close unrelated heavy workloads before running,
and publish the recorded machine/configuration details alongside these limits.
Default native cache/memtable budget is 256 MiB; local PostgreSQL shared_buffers
is 128 MiB and local MongoDB WiredTiger cache is 4 GiB. These differences must
remain visible in any publication. A tuned/equal-memory experiment is a separate
profile and requires actual enforceable limits, not relabeling these defaults.

Durability is native RocksDB Durable (WAL sync), PostgreSQL fsync/full_page_writes
on with synchronous_commit on, and standalone MongoDB w=1,j=true. No replication
equivalence is claimed. MongoDB ordered bulk writes can partially commit; other
adapters use atomic batches. Request batching is identical in logical size but
not in cross-engine transaction atomicity. Native per-operation cancellation is
unavailable; PostgreSQL measured workers have 30-second statement timeouts and
MongoDB a 30-second socket timeout. Report these differences.

Storage footprints have intentionally different scopes: isolated RocksDB directory
including WAL/metadata, PostgreSQL relation+indexes+TOAST excluding cluster WAL,
and MongoDB allocated collection+indexes excluding journal. Reports label each
scope; these figures cannot justify an unqualified storage winner.

## Artifacts, review and recovery

The result directory contains a versioned manifest, randomized schedule, config
and source hashes, source snapshot, machine metadata, native binary hash/build
details, actual engine/driver versions/settings/index plans, resource JSONL,
raw measured/warm-up histograms, ingestion summaries, exact write-oracle SQLite
files, and phase checkpoints. JSON, CSV (including ingestion), Markdown and
self-contained interactive HTML aggregate only verified measurements. Means,
sample SD and two-sided Student t 95% intervals describe independent repetition
jobs. Latency values in aggregate charts are means of per-trial quantiles, not
pooled quantiles. Single-repetition runs have no invented interval.

`protocol_passed` requires the primary size, >=5 repetitions, >=60/300-second
phases, no operation-count truncation, all engines/index scenarios, complete
planned trials, full correctness/restoration, complete resource samples, no
failed attempts, and a Release native build. It does not mean journal acceptance
or permission to publish an engine-only winner. Review scope, noise, configuration
differences, outliers and raw artifacts before making workload-specific claims.

Completed job namespaces and their owned temporary native directories are removed
automatically; results are retained. Failed/interrupted datasets and checkpoints
are retained. `--resume` requires identical config/source hashes, skips completed
jobs, and restarts each incomplete job in a **new** namespace/directory. It does
not append to a partially committed load. Old failed stores require explicit,
targeted cleanup using their recorded paths/namespaces; no broad cleanup command
is provided. Replay maps and raw resource logs can themselves use substantial disk.
Disk headroom is checked before every new load with a 5x logical-JSON heuristic;
that estimate is not a capacity guarantee. Loading stops on critically low space.

## Commands — no experiment starts from planning

Inside Ubuntu WSL, this is read-only and needs no database credentials:

```bash
cd /home/parham/NexoraDB/nexora-scale-lab
bash scripts/run_publication.sh --plan
```

When you choose to start, use Windows PowerShell. Root inside WSL is needed for
server IO/PSS access, not for database authentication. After explicit confirmation,
the launcher starts the local services and raises PostgreSQL max_connections to
600 if necessary (restarting only when no existing client sessions are present).
It then uses the existing ignored `.env.local`; passwords are not copied into
reports/source snapshots. It refuses a non-Release native artifact.

```powershell
wsl -d Ubuntu -u root -- bash /home/parham/NexoraDB/nexora-scale-lab/scripts/run_publication.sh --confirm-100m
```

To resume, inside a root WSL shell with no competing controller:

```bash
cd /home/parham/NexoraDB/nexora-scale-lab
source .env.local
export PYTHONPATH=/home/parham/NexoraDB
.venv/bin/python -m scale_lab compare --config config/publication.json \
  --directory results/THE_ORIGINAL_DIRECTORY --allow-large --resume
```

Regenerate reports without connecting to any database:

```bash
.venv/bin/python -m scale_lab report --directory results/THE_ORIGINAL_DIRECTORY
```

For a short development protocol test, explicitly select
`config/methodology-smoke.json`; these results cannot pass the publication gate.
Open-loop, Zipf and alternate working-set studies require their own JSON config
and result directory and must not be pooled with the primary uniform closed-loop
experiment. YCSB D/E/F, shared-key contention, range/group/join queries, distributed
durability, and verified cold-cache studies are **not implemented in this profile**,
not asserted unsupported by every database. They are excluded from its claims.
