# RocksDB tuning and I/O — phase 7

Implemented and measured on 2026-10-20, RocksDB 8.5.3, GCC 11.4,
Release/LTO, WSL2 Linux 6.6.87.2, ext4 on virtual `/dev/sdd`.
This is not validation on a production SSD or a power-loss test.

## API and defaults

`DocEngine(path, TransactionSettings{}, RocksSettings{})` preserves existing
constructors. Python accepts `DocEngine(path, rocks_settings=settings)`.
Settings are copied at open; changing the caller's options does not reconfigure
an open DB. `RocksResources` can be shared across engines and owns the cache and
WriteBufferManager; when supplied, its budgets override the settings' budgets.

```python
resources = nexoradb.RocksResources(256 * 1024 * 1024)
settings = nexoradb.RocksSettings()
settings.shared_resources = resources
settings.durability = nexoradb.DocumentDurability.Durable
db = nexoradb.DocEngine("db", rocks_settings=settings)
metrics = db.rocks_metrics()
```

- Default resource budget: 256 MiB, half block cache, half memtables. Cache has
  strict capacity; the shared memtable manager requests flushes at its soft
  threshold, without global WBM stalls. Pinned iterators can retain flushed
  memtables, so global WBM stalling can deadlock their owner's next write even
  when flush workers are idle. Per-CF buffer/L0 write backpressure remains.
  Migration releases its selecting iterator before committing owned keys.
  This is **not a hard process RSS cap**: pinned memtables, transient allocations,
  tables/readers, transactions, allocator and OS page cache are outside it.
- Three write buffers, each at most 16 MiB, derived and validated against the
  memtable budget. Invalid configurations fail before DB open.
- Background jobs: min(available/configured CPUs, 2 for unknown/HDD or 4 for
  SSD). Explicit overrides are CPU-clamped. No automatic disk detection.
- 4 KiB blocks; shared index/filter/data block cache; 10-bit whole-key and
  prefix Bloom filtering. L0–L2 LZ4 (Snappy optional), L3+ Zstd; dynamic level
  bytes enabled and individually configurable.
- Optional write rate limiter, disabled by default: local results do not
  establish a production latency/P99 benefit. Both bytes-per-sync settings are
  configurable, initially 1 MiB; these are not a substitute for durable writes.
- Buffered preserves `sync=false`; Durable sets `sync=true` for individual,
  batch and explicit transaction writes, with strict WAL recovery. WAL stays
  enabled. Both profiles verify read checksums and use CRC32c SST checksums;
  buffered does not mean unchecked reads. `flush_storage()` waits for memtable
  flush; it is not an alternative durability contract.
- Direct reads and direct flush/compaction I/O remain disabled in production.

## Keys and reads

Stable prefix transform `nexora.keyspace-prefix.v1` groups documents and legacy
indexes by collection, idx2 by its two length-prefixed collection/index strings,
metadata by keyspace/collection, and counters by `seq:`. Unknown or malformed
keys are outside its domain. Embedded separators in textual collection names
can produce coarser Bloom groups, never narrower ones; this does not redesign
the legacy textual key encoding.

General/maintenance scans explicitly use total-order seek and `fill_cache=false`.
Index scans opt into prefix seek only inside a valid extractor domain. Queries,
index builds, joins, and metadata enumeration use iterator upper bounds where
the scan owns a single prefix; legacy multi-prefix maintenance loops retain
explicit prefix checks and total ordering. Bound strings and slices outlive
their iterators. No assumption that prefix seek alone bounds every iterator.

Indexed candidate fetches use ordered batches of up to 64 keys through MultiGet.
An owned RocksDB snapshot spans the index scan and all document batches. Lookup
join shares one epoch across its source and target reads; `_id` lookups are
batched, other target fields reuse query execution with the same epoch. Existing
first-match inner-lookup semantics remain unchanged. Explain index-entry counts
may include a batch's lookahead beyond the requested output limit.

`rocks_metrics()` reports cache capacity/usage/pinned bytes, shared memtable
budget/usage, pending compaction bytes, SST sizes, L0 count, running work,
stopped/slowdown state, stall microseconds, Bloom useful count, cache hits/misses,
logical/WAL/flush/compaction bytes and MultiGet counts. Statistics are per DB and
reset on reopen; shared cache/memtable usage is shared, not attributable to a
single DB. Metrics are sampled, not an atomic snapshot. SST write amplification
is `(flush_write_bytes + compaction_write_bytes) / logical_write_bytes`, excludes
WAL, and can be below 1 with compression. It is not device write amplification.
Properties are queried directly, without directory traversal.

### Observability contract

`RocksSettings.statistics_enabled` defaults to true and can be disabled before
open to remove RocksDB ticker instrumentation. `RocksMetrics.statistics_enabled`
distinguishes unavailable counters from genuine zeros. With statistics disabled,
cache/WBM usage, compaction debt, SST/L0 sizes, stopped/delayed-write state and
running-work properties remain available, as do engine MultiGet counters.
Ticker-derived bytes, rates and cumulative stall time return zero, not fabricated
measurements. Options are immutable for an open engine; no concurrent toggle.

Rates are lifetime fractions, not percentages or rolling-window metrics:

- `cache_hit_rate = cache_hits / (cache_hits + cache_misses)`.
- `bloom_useful_rate = bloom_useful / (bloom_useful + bloom_full_positive)`:
  useful negative full-filter checks divided by all full-filter checks. This is
  neither a Bloom false-positive rate nor a fraction of all application queries.
- `bloom_prefix_useful_rate = bloom_prefix_useful / bloom_prefix_checked`:
  prefix-filter checks for point lookups, not iterator prefix-filter coverage.

Zero denominators return zero; raw numerator/denominator counters are exposed
so a monitoring client can compute interval rates. Concurrent reads of multiple
tickers need not describe an exactly atomic instant.

Read bytes are separated as `get_read_bytes`, `multiget_read_bytes`,
`iterator_read_bytes` (keys plus values), and `compaction_read_bytes`.
The first three are logical API reads, not disk traffic. They should not be
summed into a claimed device read-amplification ratio. Write bytes and SST WA
retain the definitions above. No filesystem traversal is added to these metrics.
To monitor compaction-induced pressure, sample debt, L0 count, running work,
write-stopped/delayed-rate and delta(stall_micros), alongside application latency.
The configurable limiter and background/buffer settings provide control knobs;
their efficacy against production latency spikes is not yet established.

## Reproducible local layout probe

```sh
cmake --build build/release --target nexora_rocks_layout_probe --parallel 2
./build/release/tests/cpp/nexora_rocks_layout_probe all
```

Native TransactionDB/WriteBatch experiment, not an end-to-end DocEngine load:
20,000 documents with 256 repeated payload bytes plus 20,000 index entries;
metadata/counters per 500-record batch. 64 MiB budget, 32 MiB cache, 1 MiB
write buffers, small file/level thresholds to induce compaction. Three fresh DBs
per mode. Four-CF cases have documents/indexes/metadata/counters plus empty
default CF, sharing cache/WBM. Mixed blocks use 16 KiB documents, 4 KiB others.
Write time excludes the separately timed blocking flush and manual compaction.
Read phases use 4,096 pseudorandom Gets then MultiGet batches of 64,
`fill_cache=false`, checksum validation. All 21 runs produced checksum 1144602.

Medians, milliseconds except the final ratio:

| Mode | Write | Flush | Compact | Get 4096 | MultiGet 4096 | SST WA |
|---|---:|---:|---:|---:|---:|---:|
| Single CF, 4 KiB | 68.628 | 24.445 | 47.301 | 8.394 | 7.294 | .2490 |
| Four logical CFs, 4 KiB | 59.354 | 177.127 | 49.867 | 11.341 | 7.224 | .1939 |
| Four logical CFs, mixed blocks | 60.649 | 162.035 | 28.009 | 10.541 | 9.383 | .1494 |
| Single CF, 16 KiB | 69.702 | 29.859 | 54.838 | 12.141 | 9.586 | .1901 |
| Static level bytes | 63.049 | 37.143 | 58.535 | 11.973 | 7.340 | .2489 |
| Rate limiter 64 MiB/s | 58.696 | 33.541 | 47.373 | 9.797 | 7.599 | .2490 |
| Direct reads + flush/compaction | 58.063 | 36.876 | 58.494 | 595.383 | 593.340 | .2494 |

Single-CF 4 KiB stall times were 14,840/27,819/37,398 us; four-CF
26,766/24,035/21,251 us. Pending compaction debt before manual compaction was
162,606 bytes for dynamic baseline versus 542,280/542,280/557,738 for static
levels. Baseline final SST size 503,969 bytes versus 507,339 four-CF and 384,924
mixed. These counters expose costs; they do not prove production tail latency.

Decision: retain single-CF on disk and default 4 KiB blocks. CF separation
reduced compressed write bytes but introduced substantial flush overhead; it
does not justify an automatic migration. No new CF format/migration was shipped.
Direct I/O was markedly worse in this WSL fixture and stays off. The user
confirmed WSL as the current target environment, completing the direct-I/O
evaluation for that target only. A different deployment needs its own hardware
benchmark before reconsidering it. Dynamic levels remain
configurable; the measured debt is encouraging, not a universal recommendation.

Limitations: tiny highly compressible dataset, warm OS page cache, fixed mode
order, only three repeats, no confidence intervals or workload P99, no sustained
mixed production traffic, and no target physical-disk benchmark. Rate-limiter,
compression, background concurrency and sync intervals still need a workload
sweep against deployment latency/durability requirements. Phase 7's global
These original layout-only runs did not satisfy the workload exit criterion.
The user subsequently authorized a standard workload in place of a private
dataset. See [the DocEngine workload matrix](ROCKSDB_WORKLOAD_BENCHMARK.md) for
42 YCSB-inspired mixed/read-heavy/read-only runs over the selected tuning knobs
and the WBM pressure fix they uncovered. Together these complete the chosen
WSL-target acceptance scope, not exhaustive parameter combinations or a
production service SLA. Larger datasets, other disks and application workloads
must be re-evaluated rather than inheriting this result.

## Validation

The `RocksTuning` tests validate budget rejection, prefix domains, profiles,
shared resources, Bloom use after flush, reopening, MultiGet counters and a
concurrent writer updating 150 documents atomically while indexed queries and
self-joins verify epoch consistency across multiple batches. Python regression
also drops settings/resource wrappers before using two shared-resource engines.

Final observability and pressure-fix runs: Debug 121/121, ASan 121/121,
UBSan 121/121, and selected fully instrumented TSan (including RocksDB
dependencies) 54/54, without suppressions.
All five RocksTuning tests also passed ten repetitions under fully instrumented
TSan, including the pinned-iterator pressure regression.
The native Python regression passed. Commands: `ctest --preset debug`,
`ctest --preset asan`, `ctest --preset ubsan`, `ctest --preset tsan-full`, and
`PYTHONPATH=build/debug python3 tests/test_native_threading_contract.py`.
The sanitizer counts cover selected test executions, not all possible races or
instrumentation of the Python runtime.

Official references: [prefix seek and total ordering](https://github.com/facebook/rocksdb/wiki/Prefix-Seek),
[shared write-buffer accounting](https://github.com/facebook/rocksdb/wiki/Write-Buffer-Manager),
[memory accounting](https://github.com/facebook/rocksdb/wiki/Memory-usage-in-RocksDB),
[tuning guide](https://github.com/facebook/rocksdb/wiki/RocksDB-Tuning-Guide).
