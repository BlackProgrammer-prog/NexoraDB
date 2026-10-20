# Threading contract (2026-10-20)

## DocEngine API coverage

One fully constructed DocEngine may be used concurrently by multiple threads.
This is a memory-safety/operation contract, **not** a promise of serializable
multi-call workflows, lock-free execution, hard latency bounds or a shared
snapshot across Document and Graph engines. The engine must outlive every call,
callback, iterator and TxHandle. Construction/destruction require external
quiescence. Do not open multiple engines on the same RocksDB directory.
C++ input objects must not be mutated while a call borrows them.

All public methods are covered below (copy construction/assignment are deleted):

| API group | Methods | Contract |
|---|---|---|
| Lifecycle | constructor, destructor | Externally serialized; join workers and finish handles first |
| Health/metrics | IsHealthy, GetRamUsageBytes, GetDiskUsageBytes | Concurrent; metrics are observations, not one atomic cross-metric snapshot |
| Collection/schema | CreateCollection, DropCollection, SetSchemaValidation | Concurrent entry; serialized per collection and exclusive at the DDL barrier |
| Index maintenance | CreateIndex, DropIndex, RebuildIndexes, ResumeIndexBuild, CleanupIndexBuild | One maintenance scope per collection, including the entire online build; same-thread nested maintenance is supported |
| FK maintenance | AddForeignKey, DropForeignKey | Collection maintenance scope plus exclusive DDL barrier |
| Metadata reads | CollectionExists, GetSchema, GetForeignKeys, GetIndexes, ListCollections | Concurrent, returned values owned by caller; separate calls need not see the same metadata epoch |
| CRUD | InsertOne, InsertMany, UpdateById, UpdateMany, DeleteById, DeleteMany | Concurrent entry, shared DDL guard; document/index/counter mutation uses RocksDB transaction locks, not a global exclusive CRUD mutex |
| Bulk CRUD | InsertManyBulk, UpdateManyBulk, DeleteManyBulk | Concurrent entry; Atomic vs OrderedChunks semantics unchanged; no whole-ingestion snapshot/atomicity in chunked mode |
| Query | FindById, FindMany, ExplainPlan, FindManyFullScanForTesting, FindPage, ClosePage, Count, Exists | Concurrent; indexed execution holds a shared DDL guard so index removal cannot invalidate its chosen path; page-session registry is mutex-protected |
| Scan/join | IterateCollection, GetDocumentRange, LookupJoin | Local iterator/options state; no cross-collection shared snapshot guarantee; callback runs synchronously and must not assume a document+graph transaction |
| Begin | BeginTransaction | Concurrent creation of independent handles; each handle holds a shared DDL guard until reset/destruction |
| Explicit transaction | CommitTransaction, RollbackTransaction, InsertOneTx, UpdateByIdTx, DeleteByIdTx, FindByIdTx | Handle belongs to one engine and its creating thread, including destruction; never share/reset/commit it concurrently or use it with another engine |
| Internal users | CreateInternalUser, GetInternalUser, UpdateInternalUser, DeleteInternalUser | Concurrent calls; existing transactional uniqueness/CRUD behavior retained |
| Internal tokens | CreateInternalAppToken, ListInternalAppTokens, DeleteInternalAppToken, IsInternalAppTokenActive | Concurrent calls; individual observations, not multi-call auth snapshots |
| Migration | MigrateLegacyDocuments | Concurrent calls serialized with the background worker through the migration mutex |
| Test hooks | SetMutationFaultPointForTesting, PauseIndexBuildAfterChunksForTesting | Atomic configuration; the operation consuming a fault under concurrent load is intentionally nondeterministic |

An explicit transaction can block DDL on unrelated collections: its shared
barrier is deliberately retained for cross-collection FK/metadata safety. Do not
invoke DDL from the thread currently owning such a handle, or recursively enter
another guarded operation from a guarded callback. Finish/reset the handle
first. A pure per-collection replacement for this barrier is **not implemented**:
it requires a complete FK dependency set and transaction-held collection locks.
Ordinary independent-collection writes already run under shared admission and
RocksDB key-level transaction locks. Adding one exclusive engine-wide CRUD lock
would reduce concurrency without fixing that dependency problem.

## Options, metadata cache and GIL

ReadOptions/WriteOptions members are const; default writes remain sync=false
(this change does not upgrade document durability). TransactionOptions are set
only before construction publishes the object. Scans use local copies for
fill_cache, snapshots and iterator bounds; referenced local bounds outlive the
iterator. Never put per-request state into a member option object.

The bounded metadata cache stores owned schema/FK/index bundles for up to 256
collections. Access and copies are mutex-protected. Every metadata-changing API
invalidates on entry and exit, even on errors. An active-writer counter disables
hits/publication throughout online maintenance; an epoch prevents an old load
from publishing after invalidation. Errors and missing collections are not
cached. Cache mutexes are not held during RocksDB I/O or lock acquisition.
Out-of-band edits with another RocksDB accessor are unsupported and cannot
trigger invalidation.

Python GIL release is not a database lock or proof of thread safety. Native
operations use the synchronization above. Metadata/index/FK operations release
the GIL while waiting/doing I/O, so they cannot block the Python thread needed to
finish an existing transaction. Schema/index/FK, Condition, UpdateSpec and bulk
options are copied before release where passed from mutable bound Python
objects. Python callback execution reacquires the GIL. A returned transaction
keeps its owning engine alive, but remains thread-confined; callers must finish
it on its creating thread. Condition/UpdateSpec and other mutable configuration
objects themselves are not general-purpose concurrently mutable C++ objects.

## Graph lifetime, admission and ordering

Registry values are shared_ptr<GraphHandle>. A lookup pins the owner while
holding registry_mutex, then releases registry_mutex **before** any operation
gate, snapshot build, algorithm run, source callback or graph/WAL access.
runLock protects manager-mediated updates for its duration; direct low-level
LiveGraph mutation bypasses that higher-level consistency contract.

`shareLiveGraph` returns an aliasing shared_ptr that pins storage/WAL as well as
LiveGraph. `acquireReadLock` returns `{pointer, GraphReadLease}`; the lease owns
both the handle and its operation lock, released in that order (lock first,
owner last). The second element is no longer a bare std::shared_lock.
Legacy `getLiveGraph` is a borrowed pointer only: external exclusion of
drop/shutdown/destruction remains required. New bindings use pinned access.

dropGraph returns false while an operation, read lease or shared LiveGraph
reference is active; retry after releasing it. It never waits for a graph lock
under registry_mutex or closes files beneath active work. Successful drop still
deletes that graph's files permanently. startup/shutdown/destruction require
external quiescence, including releasing borrowed pointers and leases; the
manager must not be destroyed while threads are invoking its methods.

Lock ordering:

1. Registry: lookup/copy owners only, then release. Lifecycle create/drop may do
   filesystem I/O under registry but never wait for a graph operation lock.
2. Per-handle operation gate: shared for reads/algorithms/snapshot, exclusive
   for build/render/compaction, definition changes and document-update dispatch.
3. Snapshot cache mutex, when needed; serializes mutable template publication.
4. LiveGraph internal gate; each public accessor/mutator owns its own guard.
5. Storage/WAL locks, acquired by established persistence paths. WAL append,
   data writes and checkpoint durability order is unchanged.

Never acquire registry from a callback executing under a LiveGraph traversal
lock; callbacks must not re-enter that graph. Algorithm bodies may use manager
registry-only queries, but must not request exclusive maintenance of their own
graph while holding a manager read lease. Snapshot uses LiveGraph's internal
gate for one consistent source epoch, not nested public accessor locking.

Document ordering is collection-maintenance gate -> shared/exclusive DDL guard
-> RocksDB transaction/key locks. Metadata-cache mutex is short-lived and is
never held across any of those waits or storage I/O. Collection gate registry
contains weak_ptrs so idle collection locks can be reclaimed. The global DDL
barrier has no strict fairness promise.

LiveGraph and the handle gate use writer-preferred admission: once a writer
queues, new readers wait rather than overtaking it. This avoids starvation from
an endless stream of readers, not arbitrary scheduler starvation or FIFO between
writers; sustained writers may delay readers. The manager gate coordinates
multi-step lifecycle/algorithms, while the LiveGraph gate protects actual data.
They are not redundant substitutes. Update dispatch takes the manager gate once,
not again inside every mapping callback. Cache publication now has its own lock;
a shared read gate alone never authorizes cache mutation.

## Sharded mutation-lock evaluation

The current dependency audit rejects a node-only sharding change: a single edge
mutation touches two adjacency lists, shared dense vector growth, shared ID/type
registries, canonical EdgeStore, counters, WAL sequencing and storage/checkpoint
state. Compaction/remap and snapshot additionally need a coherent graph epoch.
A hub defeats endpoint-local sharding even with a redesigned edge store. A safe
prototype needs deterministic endpoint shard order plus separate identity,
allocation and epoch coordination; adding several locks around the existing
global state is not such a prototype.

`nexora_graph_threading_probe` measures 1/2/4 writers on disjoint pairs and a
shared hub, 5,000 edges/writer, pre-created nodes, WAL/storage disabled. It
isolates current in-memory contention, **not** durable throughput or measured
sharding speedup. Sharding remains deliberately disabled; the requested
evaluation is an explicit design decision, not a claim that it was implemented.

Release + LTO, GCC 11.4 / WSL, medians of three fresh-process runs:

| Writers | Disjoint edges/s | Shared-hub edges/s |
|---:|---:|---:|
| 1 | 6,426,470 | 514,779 |
| 2 | 1,760,580 | 135,453 |
| 4 | 69,773 | 27,901 |

Total graph size/work grows with writer count, so this is not a fixed-size
strong-scaling experiment. Hub degree/index costs and allocator/scheduler effects
also contribute; the measurements do not isolate one mutex as the sole cause.
They demonstrate significant contention in this fixture, not a contention fix
or justification for enabling unsafe sharding. Reproduce with
`cmake --build build/benchmark --target nexora_graph_threading_probe --parallel 2`
then `build/benchmark/tests/cpp/nexora_graph_threading_probe`.

## Validation and limits

This document records the initial threading changes. The subsequent bounded
pool, streaming API, GIL audit and fully instrumented dependency build are
documented in `JOB_SCHEDULER_AND_BINDINGS.md`; consult that report for current
validation. The partial-dependency TSan limitation below is historical.

Five new ThreadingContract tests cover cache invalidation, concurrent metadata
readers/DDL and independent collection writes, registry progress during a paused
algorithm, concurrent cached snapshots, owner/read-lease drop protection, and
writer progress under continuous readers. Timeout-based progress checks are
regressions, not a mathematical proof of all possible deadlocks or scheduling.

Debug CTest: 110/110 at the initial validation. Targeted TSan LiveGraph reader/
writer and writer-progress tests: 2/2. The mixed DocEngine/RocksDB TSan run is
**not clean**: reports show Slice::compare reads against MemTable::Add writes.
The dependency build cache has CXX_FLAGS=-fPIC, DEBUG=-g and no sanitizer flags;
its lock-free synchronization is not instrumented. This supports an incomplete-
instrumentation explanation but does not prove each report benign. No broad
suppression was added and no full TSan-clean claim is made. A fully instrumented
RocksDB/dependency build is still required for that separate release gate.

ASan with leak detection and UBSan with halt-on-error each passed 74/74 graph
tests and 36/36 document tests. The Python subprocess regression passed with
`PYTHONPATH=build/debug python3 tests/test_native_threading_contract.py`; its
20-second outer timeout catches GIL-owning deadlocks without hanging the test
runner. It checks DDL waiting on a transaction while the owner can commit, and
four Python writer threads sharing one engine.
