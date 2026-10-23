# Durable ingestion: 150k target investigation

## Evidence and limits

The active `compact_20261022_safe_batch` controller was interrupted with SIGINT
after explicit user approval. Its partial Nexora store and archived MongoDB
results were retained. No 100M experiment was launched during this change.

At roughly 25.18M records / 447 seconds, the old pipeline averaged about 56.3k/s
but reported a recent window of 12.4k/s. About 98.5% of parent load time was in
batch insertion/ACK, versus about 0.8% waiting for the producer. Some slow
windows had no active compaction, no compaction debt and zero RocksDB stall
time. This job added no cgroup memory-high or OOM events. Therefore neither
Python generation nor compaction nor memory pressure alone explains this run.
The single writer issued approximately one durable WAL sync per 1,000 records;
group-commit followers were zero. Added write-path histograms expose WAL sync,
DB write and DB get latency rather than attributing all delay to compaction.

## Implemented path

- Explicit `insert_many_bson_ingest` accepts at most 10,000 documents / 64MiB.
  It is an exclusive bulk-ingestion mode, not a replacement for concurrent
  transactional writes. The DDL/mutation gate and migration gate exclude racing
  mutations while a single atomic RocksDB WriteBatch publishes documents,
  secondary indexes and the count. Normal transaction semantics remain intact.
- Fully validated BSON captures the root string `_id` during validation instead
  of traversing it again. Duplicate IDs are checked before publication.
- A fresh bounded primary-key iterator, under both gates, proves absence for
  empty collections or keys strictly beyond the current maximum. This is not
  a cached high-water mark or an unchecked overwrite. Backfills use exact
  MultiGet checks. Existing IDs fail atomically.
- Writes are grouped by keyspace. The new API acknowledges a count instead of
  constructing a large ID response. Unique indexes, schema constraints, foreign
  keys, legacy indexes and missing IDs fall back to the checked transaction path.
- Two spawn-only processes generate deterministic BSON and checksums with bounded
  prefetch. No process forks an open RocksDB instance. The parent advances
  progress only after durable ACK and combines the producer digests in order.
- The capacity profile uses 10,000-record batches and disables additional WAL
  range-sync pacing, **not** WAL or `sync=true`. Each acknowledged batch remains
  durable. Exclusive batches can delay other writers; this is a deliberate
  capacity-mode tradeoff, not a multiwriter throughput claim.

## Measurements

Artifacts are in `nexora-scale-lab/results/`. Probes include generation,
checksumming, producer startup and insertion; final flush is reported separately.
Each final run verifies exact count and 100 sampled document contents, not a
full content scan. Durability/crash, injected failure, index and concurrent
snapshot behavior also have separate regression tests.

| Probe | Records | Records/s |
| --- | ---: | ---: |
| Old transactional, batch 1,000 (`target150k_native_baseline.json`) | 300k | 71,470 |
| Transactional, batch 10,000 (`target150k_batch10000.json`) | 300k | 84,078 |
| Intermediate exclusive mode, real-length namespace (`target150k_5m_protocol_keys.json`) | 5M | 148,027 |
| Final, real-length namespace, repetition 1 | 5M | 152,316 |
| Final, real-length namespace, repetition 2 | 5M | 155,189 |
| Final, real-length namespace, repetition 3 | 5M | 157,974 |

Final artifact: `target150k_final_protocol_keys_5m.json`; median **155,189/s**.
Flush took 0.087–0.127 seconds per run. Earlier short-namespace probes reached
170k–189k/s but are not representative of the real protocol key length and must
not be substituted for these final figures. Baseline and final probes differ in
batch size, producer model and row count; they are diagnostic, not an isolated
causal speedup estimate or a fair MongoDB ranking.

This establishes the target on bounded 5M probes only. It does not establish
150k instantaneous or sustained throughput through 100M. WAL sync jitter,
larger indexes and compaction still require full-scale observation.

Final verification: 58 C++ tests, 87 API/Python tests, 44 lab tests and one native
threading regression passed (190 total).

## User-run full-scale check

```powershell
wsl -d Ubuntu -u root -- bash /home/parham/NexoraDB/nexora-scale-lab/scripts/run_ingest_capacity.sh --confirm-100m
```

This creates a fresh Nexora-only capacity study, without an overall time limit.
Do not resume an old manifest with changed code. Live progress includes stage,
ACKed records, rate, memory, last batch ACK latency and WAL sync p99. The profile
runs validation and configured workloads after loading. Archived MongoDB results
are untouched, but its 1,000-record batch study is not directly comparable with
this 10,000-record capacity profile. Publication eligibility explicitly rejects
the capacity pipeline as a matched comparison.
