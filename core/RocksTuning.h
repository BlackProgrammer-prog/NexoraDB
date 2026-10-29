#pragma once
#include <rocksdb/cache.h>
#include <rocksdb/advanced_cache.h>
#include <rocksdb/options.h>
#include <rocksdb/statistics.h>
#include <rocksdb/write_buffer_manager.h>
#include <memory>

namespace nexora::core {
enum class DocumentDurability { Buffered, Durable };
enum class DiskClass { Unknown, Rotational, SSD };
enum class HotCompression { Lz4, Snappy };
// Can be shared across engines/CFs. These are cache + memtable budgets, not RSS.
struct RocksResources {
    const size_t budget_bytes,cache_bytes,memtable_bytes;
    std::shared_ptr<rocksdb::Cache> cache;
    std::shared_ptr<rocksdb::WriteBufferManager> write_buffers;
    explicit RocksResources(size_t budget=256*1024*1024,size_t cache_budget=0);
};
struct RocksSettings {
    size_t memory_budget_bytes=256*1024*1024;
    size_t block_cache_bytes=0; // zero = half of memory budget
    size_t write_buffer_bytes=0; // zero = derive from remaining budget
    int max_write_buffers=3;
    size_t block_size=4096;
    int max_background_jobs=0; // zero = conservative disk/CPU policy
    unsigned cpu_threads=0; // zero = current Linux affinity when available, else hardware_concurrency
    DiskClass disk=DiskClass::Unknown;
    bool dynamic_level_bytes=true;
    HotCompression hot_compression=HotCompression::Lz4;
    bool cold_zstd=true;
    int64_t rate_limit_bytes_per_second=0; // zero = disabled until measured
    uint64_t bytes_per_sync=1024*1024;
    uint64_t wal_bytes_per_sync=1024*1024;
    bool bloom_filter=true;
    bool prefix_extractor=true;
    bool statistics_enabled=true;
    rocksdb::Env* env_for_testing=nullptr; // Non-owning fault-injection hook; must outlive engine.
    DocumentDurability durability=DocumentDurability::Durable;
    std::shared_ptr<RocksResources> shared_resources;
};
// A stable transform for existing textual keyspaces + length-prefixed idx2.
std::shared_ptr<const rocksdb::SliceTransform> MakeKeyspacePrefixExtractor();
rocksdb::Options MakeRocksOptions(const RocksSettings&,const std::shared_ptr<RocksResources>&);
rocksdb::WriteOptions MakeDocumentWriteOptions(DocumentDurability);
rocksdb::ReadOptions MakeDocumentReadOptions();
rocksdb::ReadOptions MakeScanReadOptions(const rocksdb::ReadOptions&);
struct RocksMetrics {
    bool statistics_enabled=false;
    uint64_t cache_capacity=0,cache_usage=0,cache_pinned=0,memtable_budget=0,memtable_usage=0;
    uint64_t pending_compaction_bytes=0,live_sst_bytes=0,total_sst_bytes=0,l0_files=0;
    uint64_t write_stopped=0,delayed_write_rate=0,running_compactions=0,running_flushes=0;
    uint64_t logical_write_bytes=0,wal_write_bytes=0,flush_write_bytes=0,compaction_write_bytes=0;
    uint64_t stall_micros=0,cache_hits=0,cache_misses=0,bloom_useful=0,multiget_calls=0,multiget_keys=0;
    double sst_write_amplification=0; // (flush+compaction)/logical bytes; excludes WAL
    uint64_t get_read_bytes=0,multiget_read_bytes=0,iterator_read_bytes=0,compaction_read_bytes=0;
    uint64_t bloom_full_positive=0,bloom_prefix_checked=0,bloom_prefix_useful=0;
    double cache_hit_rate=0,bloom_useful_rate=0,bloom_prefix_useful_rate=0;
    // Rates are fractions [0,1], zero when denominator is zero. Full Bloom
    // denominator is useful negatives + positives, NOT application requests.
};
}
