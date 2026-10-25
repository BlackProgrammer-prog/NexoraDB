# Ingestion slowdown investigation (2026-10-22)

Later measured follow-up: [150k capacity investigation](TARGET_150K_INGESTION.md)
separates WAL sync time, introduces explicit exclusive ingestion and removes
repeated absence lookups using a protected primary-range proof. Earlier
compaction-only interpretations below are not a complete diagnosis.

The active 100M run was interrupted with the user's explicit permission via
SIGINT to its verified controller PID. Both controller and worker exited;
`events.jsonl` records `run-interrupted`. No historical observations or private
partial store were deleted. Last persisted progress: 29,391,000 acknowledged
records / 528.45 seconds, average 55,617/s, latest short sample 61,908/s.
This is NOT a completed 100M measurement or resumable row-level checkpoint.

## Evidence, not a monotonic slowdown claim

Source: `nexora-scale-lab/results/compact_20261022_transport_retest/`
`job-0001/attempt-0000/{stdout.jsonl,load-resources.jsonl,progress.json}` and
the owned private store's `db/LOG` (path archived in `checkpoint.json`).

- Old stdout only has record counters roughly every five seconds. Approximate
  short-window rates were 60.6k/s initially, 50.7k/s around the middle, 56.0k/s
  later; exact historical rates cannot be reconstructed from those lines.
  The display's cumulative average falling is not proof of continuously
  decreasing instantaneous throughput.
- Worker cgroup `memory.high` and OOM-kill counters increased by **zero** over
  this run. A preexisting high-event counter of 218903 belongs to earlier
  activity, not this run. No observed new memory-throttling/OOM evidence.
- Process CPU was roughly 1.18..1.25 cores in sampled 60-second windows,
  process physical writes roughly 50..59MiB/s; these do not prove the device's
  maximum bandwidth was saturated. Native writes plus generation/checksum share
  the process and those CPU counters.
- Actual options: `write_buffer_size=16MiB`, three write buffers,
  `max_background_jobs=2`, versus 1GiB configured memtable budget.
  LOG events contain **892 completed flushes / 242 completed compactions**,
  ~11.93GiB compaction output and 177.13 cumulative compaction CPU seconds.
  Total process writes were ~27.43GiB. Tiny flushes and LSM rewrites are a
  confirmed substantial source of work; claiming they explain every slowdown
  would require phase timings and runtime stall/debt counters.
- **Important:** LOG's zero `rocksdb.stall.micros` appears in its STARTUP stats
  dump, not an end-of-run snapshot. No explicit stalling/stopping-write lines
  were found, but that alone cannot rule out all write stalls. New telemetry
  below addresses this gap. Earlier zero-stall interpretation was too strong.

The diagnosis is excess flush/compaction work plus a serial writer/generator
pipeline, with normal growth-related fluctuations; not proven RAM exhaustion,
not proven exclusively GIL, and not a network/parser bottleneck. Native load
uses `insert_many_bson`, bypassing HTTP and the NexoraQL parser/advisor.

## Changes and tradeoffs

1. Automatic write buffer ceiling increases from 16MiB to **64MiB**, still
   bounded by `memtable_budget / max_write_buffers`; explicit sizes and small
   budgets remain honored. No increase in cache/memtable total budget or cgroup
   limit. This can increase per-buffer retention and individual flush latency;
   it is not an RSS cap or universal optimal setting.
2. Native BSON binding pins strong references to immutable CPython `bytes`
   across the synchronous GIL-free call and passes `string_view`s, removing
   Python-bytes-to-string and per-document loop copies. Native validation and
   the owned versioned storage encoding remain. Mutable bytearray input is
   rejected; no borrowed input is retained after return/commit. JSON/default/
   schema/FK fallback still uses owned mutable text and identical constraints.
3. Parser syntax cache and bounded local deterministic index advisor default
   ON for new processes/engines. Explicit OFF remains honored; automatic DDL
   and remote model calls remain absent. Advisory observations start on the
   first eligible SELECT, without requiring an initial SHOW/SET. `SHOW` is a
   read command, not an on/off setting. Neither control accelerates native
   benchmark loading, which does not invoke this query layer.
4. Every five seconds and at load completion, `load-engine.jsonl` and stdout
   now record exact elapsed/average/recent rates, cumulative producer wait and
   batch/ack times, bytes flushed/compacted, pending compaction, L0 count,
   memtable/cache use and stall counters. Ack includes client/BSON encoding;
   producer work overlaps and durations must not be added as CPU attribution.
   The terminal shows debt, stalls and producer wait for native loads.
   Final `ingestion.json` includes pipeline timings. Sampling is outside the
   per-batch hot path; it still adds slight instrumentation overhead.

Durable WAL acknowledgments, atomic batches, unique/FK/schema validation,
checksums and index maintenance were not disabled. More background workers
were not forced without evidence that the existing workers cannot keep up.

## Bounded A/B results

`scripts/benchmark_ingest_buffers.py` runs at most 1M rows per case, uses the
benchmark's deterministic data/checksum/prefetch pipeline, atomic durable BSON
batches and a username index, alternating order over three repetitions. It
holds the protocol lock to avoid colliding with a real benchmark.

| Build / median records/s | 16MiB | 64MiB |
| --- | ---: | ---: |
| Before input-view changes | 66,178 | 69,240 |
| After input-view changes | 65,539 | 69,580 |
| After changes, including final synchronous memtable flush | 65,455 | 69,249 |

Artifacts: `results/ingest_buffers_1m_ab.json` and
`results/ingest_buffers_1m_owned_views.json` under `nexora-scale-lab`.
Buffer A/B gains were ~4.6% and ~6.2%, not a promised 100M gain. Compaction
output at end of load decreased from ~193.6MiB to ~55.7MiB (~71%). Both cases
reported zero pending compaction bytes after final flush in these probes;
we did not run a full explicit compaction-to-bottom-level drain. Cross-run
differences do NOT establish a standalone speedup from input views. Results
on 300k rows were near noise; larger-scale confirmation remains necessary.

Guidance: [RocksDB initial tuning](https://github.com/facebook/rocksdb/wiki/Setup-Options-and-Basic-Tuning)
describes write-buffer sizing and memory tradeoffs;
[RocksDB tuning guide](https://github.com/facebook/rocksdb/wiki/RocksDB-Tuning-Guide)
recommends statistics-driven changes and warns of workload-specific regressions.

Fresh 100M runs must use newly prepared manifests. Do not edit historical
source hashes or treat old Nexora results as measurements of this revision.
