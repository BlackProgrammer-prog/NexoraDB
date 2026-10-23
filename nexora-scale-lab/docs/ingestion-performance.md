# Ingestion investigation: 2026-10-21

These are bounded diagnostics, NOT the 100M study or publication-grade rankings.
Each row below is a median of three fresh 100,000-document runs, batch size 1000,
with username secondary index present before ingestion, durable acknowledgments,
generation and the same streaming SHA256-based checksum included. Cleanup and
sampled point-read checks are outside the timed interval. These small runs do
not establish sustained LSM/compaction throughput, full dataset integrity, or
crash recovery. Existing conformance tests separately exercise full validation,
rollback, concurrent transactions, durability settings, and schema/index rules.

| Configuration | Nexora records/s | MongoDB records/s | PostgreSQL records/s |
| --- | ---: | ---: | ---: |
| Original serial JSON loader | 31,795 | 79,759 | 41,467 |
| Optimized native BSON, still serial | 51,969 | not remeasured | not remeasured |
| Optimized BSON + two-batch prefetch, ALL engines | 74,967 | 116,728 | 40,422 |

Authoritative raw data in `results/`:

- `ingestion_before_single_encode.json`: original baseline and binary hash.
- `ingestion_optimized_serial_verified.json`: isolate native/API improvements.
- `ingestion_optimized_verified.json`: final same-pipeline three-engine probe.

Nexora end-to-end improvement is about 2.36x; native/API-only serial improvement
is about 1.63x. It has NOT beaten MongoDB with the same new loader. Comparing
Nexora's new pipeline against only MongoDB's old 79k figure would be misleading.
No throughput target is guaranteed on 100M records.

## Observed costs and changes

Originally Nexora spent about 2.27s of a 3.14s/100k run in its native insert call,
plus about .24s serializing JSON. Ingestion repeatedly encoded the same JSON for
defaults/validation, identity extraction, storage, foreign-key checking even
with no FKs, and secondary-index extraction. A bounded syscall diagnostic found
only about .019s total time in traced write/fsync/fdatasync calls, and the original
small runs reported zero RocksDB stall time. Disk sync was not the primary
observed bottleneck in that specific test; this says nothing about sustained
100M WAL/compaction costs.

Implemented:

1. Reuse validated storage encoding for JSON batch inserts, primary-ID lookup
   and index extraction. Skip FK parsing only when no FKs exist; document
   validation remains mandatory.
2. Add `insert_many_bson(list[bytes])` to the real native API. It validates BSON,
   UTF-8, field-name uniqueness/length, nesting, finite numbers, size and `_id`.
   Schema/default/FK cases fall back to the existing checked logical-document
   path. The benchmark uses PyMongo's BSON encoder, avoiding JSON round trips.
3. Compile index field paths and key prefixes once per transaction; construct
   physical keys with reserved capacity. Storage format and uniqueness locks
   remain unchanged.
4. Avoid per-field heap allocations in duplicate-name validation of small
   objects; retain hash-based duplicate checks for larger objects.
5. Compact loader now prefetches two batches with ONE generator/checksum thread
   and ONE database writer, identically for every engine. Memory is bounded by
   the queue plus one producing and one consuming batch. Only acknowledged
   batches advance progress/digest. Errors propagate; early cancellation joins
   the producer. There is no asynchronous/unacknowledged write shortcut.

Checksums run concurrently, so phase durations may overlap and must not be
summed into elapsed time. No reduction in N, removal of the username index,
increase in the 256MiB Rocks cache/memtable budget, or relaxation of WAL sync
was used. All-or-nothing batch transactions remain for Nexora/PostgreSQL;
MongoDB ordered insert_many has different failure atomicity, not an identical
transaction protocol. The remaining gap is in the insertion/transaction/index
path; its exact distribution still needs deeper native profiling, not a claim
that this gap is inherently necessary.

Intermediate diagnostic files are retained. Some intermediate probes retained
a Python bound method beyond engine shutdown; their later iterations could
include previous-engine destructor delays. The profiler now deletes that
temporary reference inside each batch. Only the final files above are used in
the final table; no failed/slow intermediate files were deleted.

## Running bounded diagnostics

Source `.env.local` inside the lab, then with the lab and repository on PYTHONPATH:

```bash
.venv/bin/python scripts/profile_ingestion.py --native-bson --prefetch 2 \
  --output results/new-diagnostic.json
```

The diagnostic refuses more than 1M documents or five repetitions per engine,
refuses overwriting output, and holds the benchmark lock. Do not run it alongside
the large study. Compact comparisons now archive native ingestion source files
as well as binary/config hashes. A cross-revision continuation preserves old
MongoDB observations explicitly as older serial-loader results, not new pipeline
measurements. Use the separately prepared optimized study, not a resume that
fakes old source hashes. No 100M experiment was started for this investigation.
