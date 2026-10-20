#include "TestTempDir.h"
#include "core/RocksTuning.h"
#include "core/IndexCodec.h"
#include <rocksdb/utilities/transaction_db.h>
#include <rocksdb/write_batch.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace nexora::core;
namespace {
void check(const rocksdb::Status& status) { if (!status.ok()) throw std::runtime_error(status.ToString()); }
template<class Fn> double milliseconds(Fn fn) {
    const auto begin=std::chrono::steady_clock::now(); fn();
    return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
}
struct Database {
    rocksdb::TransactionDB* db=nullptr;
    std::vector<rocksdb::ColumnFamilyHandle*> handles;
    ~Database() { for (auto* handle:handles) delete handle; delete db; }
};
void run(const std::string& mode,int repeat) {
    const bool split=mode.starts_with("four_cf"),direct=mode=="direct";
    TestTempDir temp("rocks_layout_probe"); RocksSettings settings;
    settings.memory_budget_bytes=64*1024*1024; settings.write_buffer_bytes=1024*1024;
    settings.block_size=mode=="block16k" ? 16384 : 4096;
    settings.dynamic_level_bytes=mode!="static_levels";
    if (mode=="rate_limited") settings.rate_limit_bytes_per_second=64*1024*1024;
    auto resources=std::make_shared<RocksResources>(settings.memory_budget_bytes);
    auto options=MakeRocksOptions(settings,resources);
    options.use_direct_reads=direct; options.use_direct_io_for_flush_and_compaction=direct;
    options.target_file_size_base=1024*1024; options.max_bytes_for_level_base=4*1024*1024;
    options.level0_file_num_compaction_trigger=2;
    std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
    descriptors.emplace_back(rocksdb::kDefaultColumnFamilyName,options);
    if (split) for (const auto* name:{"documents","indexes","metadata","counters"}) {
        auto per_cf=settings;
        per_cf.block_size=mode=="four_cf_mixed_blocks" && std::string_view(name)=="documents" ? 16384 : 4096;
        auto cf_options=MakeRocksOptions(per_cf,resources);
        cf_options.target_file_size_base=options.target_file_size_base;
        cf_options.max_bytes_for_level_base=options.max_bytes_for_level_base;
        cf_options.level0_file_num_compaction_trigger=2;
        descriptors.emplace_back(name,cf_options);
    }
    Database database;
    check(rocksdb::TransactionDB::Open(options,{},temp.path().string(),descriptors,&database.handles,&database.db));
    auto* db=database.db;
    auto cf=[&](size_t index) { return database.handles[split ? index : 0]; };
    constexpr size_t count=20000;
    std::vector<std::string> documents; documents.reserve(count);
    for (size_t i=0;i<count;++i) documents.push_back("data:items:"+std::to_string(i));
    const auto write_ms=milliseconds([&] {
        for (size_t begin=0;begin<count;begin+=500) {
            rocksdb::WriteBatch batch;
            for (size_t i=begin;i<std::min(count,begin+500);++i) {
                batch.Put(cf(1),documents[i],"{\"v\":"+std::to_string(i)+",\"payload\":\""+std::string(256,'x')+"\"}");
                batch.Put(cf(2),indexv2::EntryKey("items","group",std::to_string(i%100),std::to_string(i)),std::to_string(i));
            }
            batch.Put(cf(3),"meta:col:items","schema"); batch.Put(cf(4),"seq:items",std::to_string(begin+500));
            check(db->Write({},&batch));
        }
    });
    rocksdb::FlushOptions flush; flush.wait=true;
    const auto flush_ms=milliseconds([&] { check(db->Flush(flush,database.handles)); });
    uint64_t debt_before=0;
    for (auto* handle:database.handles) { uint64_t value=0; db->GetIntProperty(handle,"rocksdb.estimate-pending-compaction-bytes",&value); debt_before+=value; }
    const auto compact_ms=milliseconds([&] { for (auto* handle:database.handles) check(db->CompactRange({},handle,nullptr,nullptr)); });
    auto read=MakeScanReadOptions(MakeDocumentReadOptions());
    size_t checksum=0;
    const auto gets_ms=milliseconds([&] {
        for (size_t i=0;i<4096;++i) {
            std::string value; check(db->Get(read,cf(1),documents[(i*7919)%count],&value)); checksum+=value.size();
        }
    });
    size_t batch_checksum=0;
    const auto multiget_ms=milliseconds([&] {
        for (size_t begin=0;begin<4096;begin+=64) {
            std::vector<rocksdb::Slice> keys;
            for (size_t i=begin;i<begin+64;++i) keys.emplace_back(documents[(i*7919)%count]);
            std::vector<rocksdb::ColumnFamilyHandle*> handles(keys.size(),cf(1));
            std::vector<std::string> values;
            const auto statuses=db->MultiGet(read,handles,keys,&values);
            for (size_t i=0;i<statuses.size();++i) { check(statuses[i]); batch_checksum+=values[i].size(); }
        }
    });
    if (checksum!=batch_checksum) throw std::runtime_error("read checksum mismatch");
    uint64_t sst=0;
    for (auto* handle:database.handles) { uint64_t value=0; db->GetIntProperty(handle,"rocksdb.live-sst-files-size",&value); sst+=value; }
    // All CFs share the DB statistics object retained by DBOptions.
    const auto stats=options.statistics;
    const auto logical=stats->getTickerCount(rocksdb::BYTES_WRITTEN);
    const auto flushed=stats->getTickerCount(rocksdb::FLUSH_WRITE_BYTES),compacted=stats->getTickerCount(rocksdb::COMPACT_WRITE_BYTES);
    std::cout<<"mode="<<mode<<" repeat="<<repeat<<" write_ms="<<write_ms<<" flush_ms="<<flush_ms
        <<" compact_ms="<<compact_ms<<" get4096_ms="<<gets_ms<<" multiget4096_ms="<<multiget_ms
        <<" logical_bytes="<<logical<<" flush_bytes="<<flushed<<" compact_bytes="<<compacted
        <<" sst_bytes="<<sst<<" sst_WA="<<(logical ? double(flushed+compacted)/logical : 0)
        <<" debt_before_manual_compact="<<debt_before<<" stall_us="<<stats->getTickerCount(rocksdb::STALL_MICROS)
        <<" cache_usage="<<resources->cache->GetUsage()<<" checksum="<<checksum<<"\n";
}
}
int main(int argc,char** argv) {
    const std::string selected=argc>1 ? argv[1] : "all";
    for (int repeat=0;repeat<3;++repeat) for (const auto* mode:{"single_cf","four_cf","four_cf_mixed_blocks","block16k","static_levels","rate_limited","direct"}) {
        if (selected!="all" && selected!=mode) continue;
        try { run(mode,repeat); }
        catch (const std::exception& error) { std::cout<<"mode="<<mode<<" error="<<error.what()<<"\n"; if (std::string_view(mode)!="direct") return 1; }
    }
}
