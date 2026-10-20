#include "RocksTuning.h"
#include <rocksdb/filter_policy.h>
#include <rocksdb/rate_limiter.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/table.h>
#include <algorithm>
#include <stdexcept>
#include <thread>

namespace nexora::core {
RocksResources::RocksResources(size_t budget,size_t cache_budget)
    :budget_bytes(budget),cache_bytes(cache_budget ? cache_budget : budget/2),memtable_bytes(budget-cache_bytes) {
    if (budget<4*1024*1024 || cache_bytes>=budget || cache_bytes<1024*1024 || memtable_bytes<1024*1024)
        throw std::invalid_argument("RocksDB budget requires >=4MiB total, >=1MiB cache and memtables");
    cache=rocksdb::NewLRUCache(cache_bytes,-1,true);
    // Iterators can pin flushed memtables. A global WBM stall would then block
    // their owner's next write indefinitely, even with all flush workers idle.
    // Use its soft flush threshold; per-CF buffer/L0 limits retain backpressure.
    write_buffers=std::make_shared<rocksdb::WriteBufferManager>(memtable_bytes,nullptr,false);
}
namespace {
class KeyspacePrefix final:public rocksdb::SliceTransform {
    static size_t prefixSize(const rocksdb::Slice& key) {
        const std::string_view text(key.data(),key.size());
        if (text.starts_with("idx2")) {
            size_t offset=4;
            for (int field=0;field<2;++field) {
                if (offset>key.size() || key.size()-offset<4) return 0;
                uint32_t length=0;
                for (int byte=0;byte<4;++byte) length=(length<<8)|uint8_t(key[offset+byte]);
                offset+=4; if (length>key.size()-offset) return 0; offset+=length;
            }
            return offset; // collection + engine-owned index ID, not tuple bytes
        }
        size_t delimiters=0;
        if (text.starts_with("data:") || text.starts_with("idx:")) delimiters=2;
        else if (text.starts_with("meta:idx:") || text.starts_with("meta:fk:")) delimiters=3;
        else if (text.starts_with("meta:col:")) return 9;
        else if (text.starts_with("seq:")) return 4;
        else return 0;
        size_t offset=0;
        for (size_t i=0;i<delimiters;++i) {
            const auto next=text.find(':',offset); if (next==std::string_view::npos) return 0;
            offset=next+1;
        }
        return offset;
    }
public:
    const char* Name() const override { return "nexora.keyspace-prefix.v1"; }
    rocksdb::Slice Transform(const rocksdb::Slice& key) const override { return {key.data(),prefixSize(key)}; }
    bool InDomain(const rocksdb::Slice& key) const override { return prefixSize(key)!=0; }
    bool InRange(const rocksdb::Slice& key) const override { const auto n=prefixSize(key); return n && n==key.size(); }
};
}
std::shared_ptr<const rocksdb::SliceTransform> MakeKeyspacePrefixExtractor() { return std::make_shared<KeyspacePrefix>(); }
rocksdb::Options MakeRocksOptions(const RocksSettings& settings,const std::shared_ptr<RocksResources>& resources) {
    if (!resources || settings.max_write_buffers<2 || settings.max_write_buffers>16 ||
        settings.block_size<512 || settings.block_size>128*1024 || settings.max_background_jobs<0 ||
        settings.max_background_jobs>64 || settings.rate_limit_bytes_per_second<0)
        throw std::invalid_argument("invalid RocksDB tuning limits");
    const auto write_buffer=settings.write_buffer_bytes ? settings.write_buffer_bytes :
        std::min<size_t>(16*1024*1024,resources->memtable_bytes/settings.max_write_buffers);
    if (write_buffer<256*1024 || write_buffer>resources->memtable_bytes/size_t(settings.max_write_buffers))
        throw std::invalid_argument("write buffers exceed shared memtable budget");
    rocksdb::Options options;
    if(settings.env_for_testing) options.env=settings.env_for_testing;
    options.create_if_missing=true; options.create_missing_column_families=true;
    options.max_open_files=500;
    options.write_buffer_size=write_buffer; options.max_write_buffer_number=settings.max_write_buffers;
    options.write_buffer_manager=resources->write_buffers;
    const auto cpus=std::max(1U,settings.cpu_threads ? settings.cpu_threads : std::thread::hardware_concurrency());
    const auto requested=settings.max_background_jobs ? unsigned(settings.max_background_jobs) : (settings.disk==DiskClass::SSD ? 4U : 2U);
    options.max_background_jobs=int(std::min(cpus,requested));
    options.level_compaction_dynamic_level_bytes=settings.dynamic_level_bytes;
    const auto hot=settings.hot_compression==HotCompression::Snappy ? rocksdb::kSnappyCompression : rocksdb::kLZ4Compression;
    options.compression=hot;
    options.compression_per_level.assign(options.num_levels,hot);
    if (settings.cold_zstd) for (int level=3;level<options.num_levels;++level) options.compression_per_level[level]=rocksdb::kZSTD;
    options.target_file_size_base=32*1024*1024;
    options.bytes_per_sync=settings.bytes_per_sync; options.wal_bytes_per_sync=settings.wal_bytes_per_sync;
    options.paranoid_checks=true;
    if (settings.durability==DocumentDurability::Durable) options.wal_recovery_mode=rocksdb::WALRecoveryMode::kAbsoluteConsistency;
    if (settings.rate_limit_bytes_per_second)
        options.rate_limiter.reset(rocksdb::NewGenericRateLimiter(settings.rate_limit_bytes_per_second));
    rocksdb::BlockBasedTableOptions table;
    table.block_cache=resources->cache; table.block_size=settings.block_size;
    table.cache_index_and_filter_blocks=true; table.cache_index_and_filter_blocks_with_high_priority=true;
    table.checksum=rocksdb::ChecksumType::kCRC32c;
    if (settings.bloom_filter) table.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10,false));
    table.whole_key_filtering=true;
    options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table));
    if (settings.prefix_extractor) options.prefix_extractor=MakeKeyspacePrefixExtractor();
    if (settings.statistics_enabled) {
        options.statistics=rocksdb::CreateDBStatistics();
        options.statistics->set_stats_level(rocksdb::StatsLevel::kExceptDetailedTimers);
    }
    // Direct I/O remains off: enable only in the standalone hardware probe.
    options.use_direct_reads=false; options.use_direct_io_for_flush_and_compaction=false;
    return options;
}
rocksdb::WriteOptions MakeDocumentWriteOptions(DocumentDurability profile) {
    rocksdb::WriteOptions options; options.sync=profile==DocumentDurability::Durable; return options;
}
rocksdb::ReadOptions MakeDocumentReadOptions() {
    rocksdb::ReadOptions options; options.verify_checksums=true;
    // Cross-keyspace and maintenance iterators must preserve total ordering.
    options.total_order_seek=true; return options;
}
rocksdb::ReadOptions MakeScanReadOptions(const rocksdb::ReadOptions& base) {
    auto options=base; options.fill_cache=false; options.total_order_seek=true; return options;
}
}
