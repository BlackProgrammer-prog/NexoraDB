#include "TestTempDir.h"
#include "core/DocEngine.h"
#include "core/IndexCodec.h"
#include <rocksdb/slice_transform.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <rocksdb/write_batch.h>
using namespace nexora::core;
using namespace nexora::query;

TEST(RocksTuning, BudgetOptionsProfilesAndPrefixDomain) {
    EXPECT_THROW(RocksResources(1024),std::invalid_argument);
    EXPECT_THROW(RocksResources(8*1024*1024,9*1024*1024),std::invalid_argument);
    auto resources=std::make_shared<RocksResources>(16*1024*1024);
    RocksSettings settings; settings.cpu_threads=2; settings.disk=DiskClass::SSD;
    const auto options=MakeRocksOptions(settings,resources);
    EXPECT_EQ(options.max_background_jobs,2); EXPECT_TRUE(options.level_compaction_dynamic_level_bytes);
    EXPECT_LE(options.write_buffer_size*options.max_write_buffer_number,resources->memtable_bytes);
    EXPECT_EQ(options.compression_per_level[0],rocksdb::kLZ4Compression);
    EXPECT_EQ(options.compression_per_level.back(),rocksdb::kZSTD);
    EXPECT_FALSE(options.use_direct_reads); EXPECT_FALSE(options.use_direct_io_for_flush_and_compaction);
    EXPECT_TRUE(MakeDocumentWriteOptions(DocumentDurability::Durable).sync);
    EXPECT_FALSE(MakeDocumentWriteOptions(DocumentDurability::Buffered).sync);
    EXPECT_TRUE(MakeDocumentReadOptions().verify_checksums);
    EXPECT_TRUE(MakeScanReadOptions(MakeDocumentReadOptions()).total_order_seek);
    EXPECT_FALSE(MakeScanReadOptions(MakeDocumentReadOptions()).fill_cache);
    EXPECT_NE(options.statistics,nullptr);
    settings.statistics_enabled=false;
    EXPECT_EQ(MakeRocksOptions(settings,resources).statistics,nullptr);
    settings.write_buffer_bytes=resources->memtable_bytes;
    EXPECT_THROW(MakeRocksOptions(settings,resources),std::invalid_argument);
    const auto prefix=MakeKeyspacePrefixExtractor();
    EXPECT_EQ(prefix->Transform("data:users:a").ToString(),"data:users:");
    EXPECT_EQ(prefix->Transform("meta:idx:users:name").ToString(),"meta:idx:users:");
    EXPECT_FALSE(prefix->InDomain("idx2")); EXPECT_FALSE(prefix->InDomain("other"));
    EXPECT_FALSE(prefix->InDomain(std::string("idx2\xff\xff\xff\xff",8)));
    const auto physical=indexv2::EntryKey(std::string("c:\0x",4),"identity","tuple","id");
    ASSERT_TRUE(prefix->InDomain(physical));
    EXPECT_EQ(prefix->Transform(physical).ToString(),indexv2::IndexPrefix(std::string("c:\0x",4),"identity"));
    EXPECT_TRUE(prefix->InRange(prefix->Transform(physical)));
}
TEST(RocksTuning, SharedResourcesMetricsMultigetAndReopen) {
    TestTempDir temp("rocks_resources"); auto resources=std::make_shared<RocksResources>(16*1024*1024);
    RocksSettings settings; settings.shared_resources=resources; settings.durability=DocumentDurability::Durable;
    {
        DocEngine first((temp.path()/"one").string(),{},settings),second((temp.path()/"two").string(),{},settings);
        ASSERT_TRUE(first.CreateCollection("items").success); ASSERT_TRUE(second.CreateCollection("items").success);
        std::vector<std::string> documents;
        for (int i=0;i<150;++i) documents.push_back("{\"_id\":\"n"+std::to_string(i)+"\",\"self\":\"n"+std::to_string(i)+"\",\"group\":\"g\"}");
        ASSERT_TRUE(first.InsertMany("items",documents).success);
        IndexDefinition index; index.index_name="group"; index.fields={"group"};
        ASSERT_TRUE(first.CreateIndex("items",index).success);
        ASSERT_TRUE(first.FlushStorage().success);
        for (int i=0;i<100;++i) EXPECT_FALSE(first.FindById("items","n"+std::to_string(i)+"x").success);
        const auto query=first.FindMany("items",Condition::Leaf("group",Op::EQ,"g"));
        ASSERT_TRUE(query.success)<<query.error_msg;
        EXPECT_EQ(nlohmann::json::parse(query.data).size(),150U);
        const auto join=first.LookupJoin("items","self","items","_id",{},0);
        ASSERT_TRUE(join.success)<<join.error_msg; EXPECT_EQ(join.records.size(),150U);
        ASSERT_TRUE(first.FindById("items","n0").success);
        ASSERT_TRUE(first.FindById("items","n0").success);
        const auto a=first.GetRocksMetrics(),b=second.GetRocksMetrics();
        EXPECT_TRUE(a.statistics_enabled);
        EXPECT_GT(a.get_read_bytes,0U); EXPECT_GT(a.multiget_read_bytes,0U);
        EXPECT_GT(a.iterator_read_bytes,0U);
        EXPECT_GT(a.cache_hits+a.cache_misses,0U);
        EXPECT_DOUBLE_EQ(a.cache_hit_rate,double(a.cache_hits)/(double(a.cache_hits)+double(a.cache_misses)));
        EXPECT_DOUBLE_EQ(a.bloom_useful_rate,double(a.bloom_useful)/(double(a.bloom_useful)+double(a.bloom_full_positive)));
        EXPECT_GE(a.bloom_prefix_useful_rate,0.0); EXPECT_LE(a.bloom_prefix_useful_rate,1.0);
        EXPECT_DOUBLE_EQ(a.sst_write_amplification,
            (double(a.flush_write_bytes)+double(a.compaction_write_bytes))/double(a.logical_write_bytes));
        EXPECT_EQ(a.cache_capacity,resources->cache_bytes); EXPECT_EQ(a.cache_capacity,b.cache_capacity);
        EXPECT_LE(a.cache_usage,a.cache_capacity); EXPECT_EQ(a.memtable_budget,resources->memtable_bytes);
        EXPECT_GE(a.multiget_calls,6U); EXPECT_EQ(a.multiget_keys,300U);
        EXPECT_GT(a.logical_write_bytes,0U); EXPECT_GT(a.wal_write_bytes,0U); EXPECT_GT(a.flush_write_bytes,0U);
        EXPECT_GT(a.live_sst_bytes,0U);
        EXPECT_GT(a.bloom_useful,0U); EXPECT_GE(a.l0_files,1U);
    }
    DocEngine reopened((temp.path()/"one").string(),{},settings);
    EXPECT_TRUE(reopened.FindById("items","n149").success);
    EXPECT_EQ(reopened.GetIndexes("items").size(),1U);
}
TEST(RocksTuning, DisabledStatisticsRetainsPropertiesAndSafeEmptyRates) {
    TestTempDir temp("rocks_no_stats"); RocksSettings settings;
    settings.statistics_enabled=false;
    DocEngine engine(temp.path().string(),{},settings);
    auto metrics=engine.GetRocksMetrics();
    EXPECT_FALSE(metrics.statistics_enabled);
    EXPECT_EQ(metrics.cache_hit_rate,0); EXPECT_EQ(metrics.bloom_useful_rate,0);
    EXPECT_EQ(metrics.bloom_prefix_useful_rate,0); EXPECT_EQ(metrics.sst_write_amplification,0);
    ASSERT_TRUE(engine.CreateCollection("items").success);
    ASSERT_TRUE(engine.InsertOne("items","{\"_id\":\"one\",\"value\":42}").success);
    ASSERT_TRUE(engine.FlushStorage().success);
    ASSERT_TRUE(engine.FindById("items","one").success);
    metrics=engine.GetRocksMetrics();
    EXPECT_GT(metrics.cache_capacity,0U); EXPECT_GT(metrics.memtable_budget,0U);
    EXPECT_GT(metrics.live_sst_bytes,0U); EXPECT_GE(metrics.l0_files,1U);
    EXPECT_EQ(metrics.logical_write_bytes,0U); EXPECT_EQ(metrics.get_read_bytes,0U);
}
TEST(RocksTuning, PinnedIteratorsDoNotGloballyStallWrites) {
    TestTempDir temp("rocks_pinned_memtables");
    auto resources=std::make_shared<RocksResources>(8*1024*1024);
    // Guard against hanging on a regression before exercising actual writes.
    const auto retained=resources->memtable_bytes+1;
    resources->write_buffers->ReserveMem(retained);
    const bool would_stall=resources->write_buffers->ShouldStall();
    resources->write_buffers->ScheduleFreeMem(retained);
    resources->write_buffers->FreeMem(retained);
    ASSERT_FALSE(would_stall);
    RocksSettings settings; settings.write_buffer_bytes=512*1024;
    const auto options=MakeRocksOptions(settings,resources);
    rocksdb::DB* raw=nullptr;
    ASSERT_TRUE(rocksdb::DB::Open(options,temp.path().string(),&raw).ok());
    std::unique_ptr<rocksdb::DB> db(raw);
    std::vector<std::unique_ptr<rocksdb::Iterator>> pinned;
    for(int batch=0;batch<32;++batch) {
        rocksdb::WriteBatch writes;
        for(int i=0;i<64;++i) writes.Put("data:items:"+std::to_string(batch*64+i),std::string(4096,'a'+batch%26));
        ASSERT_TRUE(db->Write({},&writes).ok());
        pinned.emplace_back(db->NewIterator(MakeDocumentReadOptions()));
        pinned.back()->SeekToFirst();
        ASSERT_TRUE(pinned.back()->Valid());
    }
    EXPECT_GT(resources->write_buffers->memory_usage(),resources->memtable_bytes);
    pinned.clear();
    rocksdb::FlushOptions flush; flush.wait=true;
    ASSERT_TRUE(db->Flush(flush).ok());
}
TEST(RocksTuning, ReadEpochSpansIndexMultigetAndJoinBatches) {
    TestTempDir temp("rocks_epoch"); DocEngine engine(temp.path().string());
    ASSERT_TRUE(engine.CreateCollection("items").success);
    std::vector<std::string> documents;
    for (int i=0;i<150;++i) documents.push_back("{\"_id\":\"n"+std::to_string(i)+"\",\"self\":\"n"+std::to_string(i)+"\",\"group\":\"g\",\"v\":0}");
    ASSERT_TRUE(engine.InsertMany("items",documents).success);
    IndexDefinition index; index.index_name="group"; index.fields={"group"}; ASSERT_TRUE(engine.CreateIndex("items",index).success);
    std::atomic<int> failures=0; std::atomic<bool> start=false;
    std::thread writer([&] {
        while (!start.load()) std::this_thread::yield();
        for (int version=1;version<=8;++version) {
            auto transaction=engine.BeginTransaction();
            UpdateSpec update; update.Set("v",std::to_string(version),UpdateValueType::Int64);
            for (int i=0;i<150;++i) if (!engine.UpdateByIdTx(*transaction,"items","n"+std::to_string(i),update).success) ++failures;
            if (!engine.CommitTransaction(*transaction).success) ++failures;
        }
    });
    start=true;
    for (int attempt=0;attempt<30;++attempt) {
        const auto result=engine.FindMany("items",Condition::Leaf("group",Op::EQ,"g"));
        if (!result.success) { ++failures; continue; }
        const auto rows=nlohmann::json::parse(result.data);
        if (rows.size()!=150) { ++failures; continue; }
        for (const auto& row:rows) if (row["v"]!=rows[0]["v"]) ++failures;
        const auto join=engine.LookupJoin("items","self","items","_id",{},0);
        if (!join.success || join.records.size()!=150) { ++failures; continue; }
        for (const auto& record:join.records) {
            const auto row=nlohmann::json::parse(record);
            if (row["v"]!=row["__joined__"]["v"]) ++failures;
        }
    }
    writer.join(); EXPECT_EQ(failures,0);
}
