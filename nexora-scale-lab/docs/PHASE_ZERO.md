# Phase zero: repeatable diagnostic baseline

Historical results are immutable evidence, not measurements of the current build:
`compact_20261021_144702` contains the archived MongoDB run;
`compact_20261022_081651` contains the Nexora capacity run. Do not edit their
manifests, source hashes or settings. Keep the cleanup intervention in the
interpretation of the latter. Existing experiment snapshots retain source bytes,
host commit/dirty status, configuration hash and engine binary/settings metadata.

## Short baseline

```powershell
wsl -d Ubuntu -u root -- bash /home/parham/NexoraDB/nexora-scale-lab/scripts/run_baseline_short.sh --plan
wsl -d Ubuntu -u root -- bash /home/parham/NexoraDB/nexora-scale-lab/scripts/run_baseline_short.sh --run
```

The first command only plans. The second creates a NEW directory and runs 1M
records per job, five independent jobs per engine, shuffled engine order,
identical seed, 1,000-document batches, one writer and the same bounded threaded
producer pipeline. The same username index exists before loading. Full validation
is enabled. It is not the native-exclusive/process-producer capacity profile.
There are 120 measurement cells (about 40 minutes of warmup/measurement alone,
plus load, full validation, restoration and cleanup). No automatic 100M follows.

Durability is acknowledged local persistence: native RocksDB WAL sync=true and
MongoDB w=1,j=true. Neither offers replicated durability in this setup; their
internal flush/grouping implementations are not identical. PostgreSQL is not
included in this first diagnostic pair.

This is a local-deployment comparison, NOT equal engine-only overhead: Nexora is
embedded and Mongo uses loopback TCP. The existing resource sampler reports
separate Mongo server/generator CPU, IO and PSS. Native CPU/PSS is combined because
both share one process; do not subtract guessed Python memory or sum overlapping
groups. Native memory budget is 1GiB/cache 512MiB for this small probe; Mongo's
actual cache/settings are recorded but are NOT asserted equal. This profile is
for same-engine before/after regressions, not a fair memory-constrained ranking
or evidence about 100M behavior.

## Audit and variability

From the lab directory, with its venv and PYTHONPATH configured:

```bash
.venv/bin/python -m scale_lab.baseline results/baseline_TIMESTAMP
```

This read-only command prints configuration/source hashes, artifact checksums and
per-cell repetition count, mean, median, sample SD and coefficient of variation
(CV). It excludes non-completed jobs and never rewrites historical artifacts.
Keep the output outside the historical result directory if archiving it.

Before accepting a speedup:

1. Require all five independent repetitions, zero failed operations and successful
   integrity/restoration checks. Inspect the normal report as well as the audit.
2. Inspect resource JSONL for memory.high/max/OOM deltas, swap and competing host
   load. A completed run alone does not establish an uncontaminated measurement.
3. Compare the same configuration hash before/after, documenting intentionally
   changed source/binary hashes. Never resume across source changes.
4. Inspect CV and per-trial distributions. An improvement smaller than the
   observed variability is inconclusive, not a confirmed win. Repeat an A/B/A
   sequence if environmental drift is suspected.
5. Small warm-conditioned datasets cannot predict out-of-cache 100M performance.

The instrumentation/profile are ready; a valid baseline is NOT established until
the short experiment actually runs and these checks pass. Historical Mongo/Nexora
results remain diagnostic and must not be pooled with new repetitions.
