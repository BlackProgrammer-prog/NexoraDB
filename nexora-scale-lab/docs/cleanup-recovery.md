# Cleanup failure and recovery, 2026-10-22 (Asia/Tehran)

The 100M Nexora job in `results/compact_20261021_optimized` finished loading in
2412.713 seconds (41,447 records/s) and saved all 12 workload trials with status
`verified-and-restored` and zero failed operations. Validation in this compact
study is SAMPLED, not full-dataset verification. MongoDB's 12 archived trials
remain unchanged. PostgreSQL had not started.

The old `DropCollection` built a point-delete WriteBatch for every document and
secondary-index key before committing anything. It exceeded the worker group's
6GiB memory.high, approached its 8GiB memory.max, used about 1GiB swap and spent
hours in cgroup reclaim/throttling. Inspection showed high events but no OOM
kills. This was a cleanup algorithm problem, not slow workload measurement.

## Recovery performed with user authorization

`scripts/recover_cleanup.py --apply` validated the complete planned trial set,
dataset size, zero failed operations, private ownership marker and exact running
controller/worker command lines. It saved the original checkpoint and measurement
SHA256 hashes in `cleanup-recovery.json`, interrupted the controller (which
terminated its cleanup worker), acquired the protocol lock and rechecked ownership
and every measurement hash before removal. The exact removed directory was:

`state/protocol/trial_igust0kq`

It contained 8,186,174,464 logical file bytes (about 7.62GiB). The test dataset is
not recoverable from that directory but can be regenerated from its recorded
seed. No result, measurement, source snapshot, MongoDB data or unrelated project
directory was deleted. Other interrupted stores were not removed. The checkpoint
records external cleanup recovery; it is NOT represented as a clean worker exit.
Publication gates explicitly reject externally recovered cleanup studies.

## Changes

- Core DropCollection now commits five bounded range tombstones and two exact
  metadata deletes in ONE durable atomic WriteBatch instead of O(record count)
  point tombstones. An exclusive DDL gate excludes foreground writers and open
  transactions; the migration mutex excludes background migration. Only this
  provably non-conflicting batch uses TransactionDB's required
  `skip_concurrency_control` range-write option. Transaction checks elsewhere
  remain enabled. On-disk obsolete bytes are reclaimed later by compaction.
- Successful native benchmark jobs no longer logically drop their temporary
  collection. They close/sync the engine, then remove ONLY their directly owned
  private directory after checking its ownership marker and exact parent root.
  New dashboard stages distinguish `closing-engine` and `removing-private-store`.
  Failed jobs retain their private stores for investigation.
- Explicit `prepare-continuation --preserve-completed` preserves all matching,
  completed and verified engine jobs. Nested Mongo import provenance is retained,
  along with the native cleanup recovery receipt. Only PostgreSQL remains to run;
  it is NOT started by preparation. Changed sources require a NEW study directory,
  never forged hashes in the original manifest.

The native regression test drops 40,000 indexed documents under an 8MiB Rocks
cache/memtable budget, bounds delete WAL bytes below 16KiB, checks neighbor
isolation, name/key reuse and reopening. Harness tests assert that private native
jobs never call logical DropCollection, and validate preserved imports/hashes and
skipping already-completed jobs. These checks are not another 100M run and do
not establish a new sustained ingestion performance figure.
