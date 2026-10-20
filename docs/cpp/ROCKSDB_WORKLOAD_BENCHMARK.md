# DocEngine workload benchmark — WSL target

The user has no application dataset and authorized choosing a database benchmark
workload. We use YCSB-inspired A/B/C mixes through the **real DocEngine API**:
50/50 reads/updates, 95/5 reads/updates, and read-only. The reference workloads
use ten 100-byte fields and Zipfian requests: [A](https://github.com/brianfrankcooper/YCSB/blob/master/workloads/workloada),
[B](https://github.com/brianfrankcooper/YCSB/blob/master/workloads/workloadb),
[C](https://github.com/brianfrankcooper/YCSB/blob/master/workloads/workloadc).
This is a native adaptation, not an official Java YCSB run or cross-database
ranking. No application-specific SLA is invented.

## Reproduction and scope

```sh
cmake --preset release
cmake --build build/release --target nexora_rocks_workload_probe --parallel 2
./build/release/tests/cpp/nexora_rocks_workload_probe all 50000 50000 3
```

Each run creates a new temporary DB, loads 50,000 documents in 500-document
batches, waits for memtable flush, warms 2,000 reads, then executes 50,000
operations on four closed-loop native workers. Each initial field is an
independent deterministic 100-byte alphanumeric string; updates replace a random
field with a newly generated string, not an identical no-op or repeated payload.
The client uses deterministic seeds and a permuted rank CDF with exponent .99,
not YCSB's exact Java scrambled-Zipfian generator. Operation choices repeat in
100-operation blocks with exact mix proportions; they are intentionally not
Poisson arrivals. Timing covers the native API, not Python/network conversion.
Random key/update preparation is outside per-operation latency but inside total
throughput time. Workers are joined before throughput ends.

Budget is 32 MiB (16 MiB cache + 16 MiB soft memtable threshold), deliberately
smaller than the approximately 50 MiB payload. OS page cache is not bounded or
dropped. Block cache is strict; pinned memtables can exceed the soft threshold.
All other defaults come from RocksSettings, with one setting changed per profile.
The limiter profile is 8 MiB/s. Durable is a separate durability contract, not
a faster/slower equivalent configuration. Three repeats rotate profile order;
baseline also runs B/C, all other profiles run A.

P50/P95/P99/max are recorded separately for reads and writes, with raw operation
and error counts. A 5 ms sampler records peak pending-compaction debt, L0 files,
and stopped/slowdown samples; cumulative stall microseconds and flush/compaction
byte deltas supplement it. Sampling can miss brief states. Closed-loop latency
can hide queueing/coordinated omission; this is not an open-loop service SLA.
Background compaction is not forcibly drained after the operation phase, so
these deltas are observations of that window, not steady-state device WA.
No secondary index/join workload is included here; those have separate
correctness tests and candidate MultiGet measurements.

## Pressure issue found and fixed

The first larger load stopped making progress: writer stacks waited in
`WriteBufferManagerStallWrites`, flush workers were idle, and the migration
thread was trying to commit while retaining its selecting iterator. This was
not recorded as a successful throughput run. We now release that iterator before
commit and disable global WBM stalls: pinned/flushed memtables cannot safely be
treated as reclaimable on demand. WBM remains a shared soft flush threshold,
while normal per-CF buffer/L0 backpressure remains enabled. A regression holds
many native iterators across writes exceeding the soft memtable threshold.
This explicitly does not promise a hard RSS/memtable cap with arbitrary readers.

## Results

Completed 2026-10-20: 42 fresh-DB executions, 2,100,000 measured API operations,
zero API errors. Each fresh load adds 50,000 documents. Host CPU is Intel
Core i9-14900K, 32 logical CPUs visible to WSL; GCC 11.4 Release/LTO and
RocksDB 8.5.3. No other benchmark/sanitizer builds ran during this matrix.
Raw per-run results: [JSONL](../../tests/benchmarks/rocks_phase7_workload_wsl.jsonl).

Medians of three independent runs. P99 values are medians of per-run P99s,
not a merged histogram. Stall time is the cumulative delta in the run window.
Disabled statistics means **unavailable**, not zero observed stalls.

| Profile/workload | ops/s | Read P99 µs | Write P99 µs | Stall µs |
|---|---:|---:|---:|---:|
| baseline/A | 108,056 | 38.8 | 127.3 | 0 |
| baseline/B | 162,932 | 34.6 | 85.8 | 0 |
| baseline/C | 179,502 | 32.4 | 0.0 | 0 |
| rate8m/A | 33,405 | 38.3 | 141.7 | 1,055,438 |
| block16k/A | 100,977 | 43.8 | 132.4 | 7,425 |
| static/A | 107,598 | 37.8 | 123.1 | 24,318 |
| jobs1/A | 104,455 | 39.9 | 121.8 | 23,365 |
| buffer1m/A | 24,809 | 51.7 | 193.7 | 1,525,982 |
| sync0/A | 113,872 | 37.3 | 115.0 | 0 |
| sync4m/A | 103,132 | 39.4 | 118.9 | 44,487 |
| snappy/A | 107,870 | 39.7 | 128.8 | 23,073 |
| nozstd/A | 103,084 | 38.6 | 116.4 | 29,574 |
| stats_off/A | 104,879 | 38.2 | 127.9 | 0 |
| durable/A | 2,099 | 54.7 | 9594.9 | 0 |

The baseline A maximum run-window stall was 22,044 µs. The limiter's median
stall was 1,055,438 µs, while 1 MiB write buffers produced 1,525,982 µs.
Static levels reached 66,633,477 pending bytes, versus 19,342,600 for baseline.
Read-only/read-heavy runs did not exercise comparable compaction pressure.
Durable writes had about 9.6 ms median-run P99; that is the cost of a different
sync contract on this virtual disk, not permission to silently weaken durability.

Decisions: preserve 4 KiB blocks, automatic buffers and dynamic levels; leave
the optional limiter off. A smaller write buffer or artificially slow limiter
is counterproductive here. There is no repeatable reason to disable Statistics,
switch to Snappy, remove Zstd, reduce jobs, or change the 1 MiB sync intervals.
`sync0` was slightly faster locally but cannot justify removing incremental
I/O smoothing from a short closed-loop sample. No default is changed merely
because one median improves. Monitoring and configuration control the observed
WSL pressure; these are not guarantees of bounded latency for every workload.

## Interpretation limits

The matrix exercises compaction pressure, sync intervals, compression choices,
background concurrency, buffer/block sizes, statistics overhead and optional
rate limiting on the chosen WSL target. It does not prove that all configurations
or cold L3+ compression have been exhaustively tuned, nor sustained service
latency on a physical disk. In particular a short run may not reach cold levels.
Defaults should change only for a repeatable benefit with matching semantics;
durability must not be weakened to improve a benchmark number.
