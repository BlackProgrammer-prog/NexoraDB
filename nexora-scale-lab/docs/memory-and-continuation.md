# Memory containment and preserved MongoDB continuation

The interrupted PostgreSQL load does not establish a PostgreSQL private-memory
leak. Its last valid resource sample recorded roughly 162 MiB server PSS and
30 MiB generator PSS. WSL termination has no confirmed root cause. Process RSS
does not account for all Linux filesystem cache or unrelated workloads.

`scripts/system/apply-postgres-profile.sh` installs the durable PostgreSQL 14
profile: shared_buffers 512MB, work_mem 4MB, maintenance_work_mem 128MB,
max_wal_size 8GB, checkpoint_timeout 15min, completion_target 0.9. fsync,
full_page_writes and synchronous_commit remain on. max_wal_size is a soft WAL
target, not a RAM cap. See the [PostgreSQL WAL documentation](https://www.postgresql.org/docs/14/runtime-config-wal.html).

Compact runs now require cgroup-v2 containment. PostgreSQL and benchmark workers
have SEPARATE groups, each with memory.high=6 GiB, memory.max=8 GiB and at most
1 GiB swap. This is up to 16 GiB combined RAM, not a shared 8 GiB budget.
The worker group includes the embedded NexoraDB engine. These limits include
charged filesystem cache, not merely process-private RAM. Above memory.high,
Linux reclaims/throttles; memory.max can cause a group-local OOM. Limits do not
guarantee that an arbitrarily large experiment will complete. A load also stops
if Linux MemAvailable falls below 1 GiB. The dashboard and raw resource logs
record guest available memory and PostgreSQL cgroup usage/events. See the
[kernel cgroup documentation](https://www.kernel.org/doc/html/latest/admin-guide/cgroup-v2.html).

The requested order is fixed: MongoDB, NexoraDB, PostgreSQL. Fixed order is not
randomized publication methodology. `prepare-continuation` copies only complete,
verified MongoDB artifacts into a NEW study, verifies every copied file by SHA256,
and archives original source/config provenance. The original study, including
interrupted PostgreSQL attempts, is untouched. Resume skips imported complete
MongoDB jobs; NexoraDB runs next, then PostgreSQL starts a fresh load.

Preserved MongoDB observations predate the new instrumentation/resource settings.
Reports disclose this mixed-revision provenance and cannot pass publication gates.
No performance comparison should present this continuation as identical engine
resource configurations. Full publication requires a new controlled run.

Preparation does not start database work. Actual 100M stability remains untested
by the implementation checks; these use only tiny real-engine datasets.

## Nexora compaction failure (2026-10-21)

The first reordered attempt failed near one million records, not 100M: its
RocksDB LOG reports `Memory limit reached: Insert failed due to LRU cache being
full` while verifying a compacted SST. Nexora's strict sharded LRU previously
stored monolithic index/filter blocks; the failed output's index alone was
2,456,823 bytes. This is an internal cache admission error, not evidence of
guest RAM exhaustion. The engine now uses two-level partitioned indexes and
partitioned Bloom filters with 4KiB metadata targets. Strict cache capacity,
the existing 256MiB cache/memtable budget and durable writes remain unchanged.
See [RocksDB partitioned index/filter documentation](https://github.com/facebook/rocksdb/wiki/Partitioned-Index-Filters).
The regression test flushes/compacts 40,000 long-key records under a strict
1MiB cache, then verifies reads. Shutdown failures no longer overwrite the
original failure stage; native error text is retained in checkpoint.json.

The corrected binary and runner use another explicit continuation directory;
older failures and MongoDB artifacts remain intact. This is not a 100M success
claim, and imported MongoDB remains exploratory mixed-revision evidence.
