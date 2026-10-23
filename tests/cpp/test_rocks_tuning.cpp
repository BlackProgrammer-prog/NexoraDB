#include "TestTempDir.h"
#include "core/DocEngine.h"
#include "core/IndexCodec.h"
#include "core/DocumentCodec.h"
#include <rocksdb/slice_transform.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <rocksdb/write_batch.h>
#include <rocksdb/table.h>
#include <bson/bson.h>
using namespace nexora::core;
using namespace nexora::query;

TEST(RocksTuning, UnchangedIndexTuplesAvoidRewriteAndQueryControlsKeepConstraints) {
    TestTempDir temp("smart_update_index_tuning");
    DocEngine engine(temp.path().string());
    ASSERT_TRUE(engine.CreateCollection("items").success);
    IndexDefinition index; index.index_name="unique_name"; index.fields={"name"}; index.type=IndexType::Unique;
    ASSERT_TRUE(engine.CreateIndex("items",index).success);
    const std::string name(200,'a'), replacement(200,'b');
    ASSERT_TRUE(engine.InsertOne("items",nlohmann::json({{"_id","one"},{"name",name},{"status","a"}}).dump()).success);
    auto before=engine.GetRocksMetrics().logical_write_bytes;
    UpdateSpec update; update.Set("status","b");
    ASSERT_TRUE(engine.UpdateById("items","one",update).success);
    const auto unchanged=engine.GetRocksMetrics().logical_write_bytes-before;
    before=engine.GetRocksMetrics().logical_write_bytes;
    UpdateSpec rename; rename.Set("name",replacement);
    ASSERT_TRUE(engine.UpdateById("items","one",rename).success);
    const auto changed=engine.GetRocksMetrics().logical_write_bytes-before;
    EXPECT_LT(unchanged,changed);
    for (bool enabled: {false,true}) {
        engine.ConfigureQueryTuning(enabled,enabled);
        auto tuning=nlohmann::json::parse(engine.QueryTuning());
        EXPECT_EQ(tuning["smart_indexes"],enabled);
        auto found=engine.FindMany("items",Condition::Leaf("name",Op::EQ,replacement),10,0);
        ASSERT_TRUE(found.success)<<found.error_msg;
        EXPECT_EQ(nlohmann::json::parse(found.data).size(),1U);
        auto plan=engine.ExplainPlan("items",Condition::Leaf("name",Op::EQ,replacement));
        ASSERT_TRUE(plan.success)<<plan.error_msg;
        EXPECT_EQ(nlohmann::json::parse(plan.data)["scan_type"]=="full_scan",!enabled);
        EXPECT_FALSE(engine.InsertOne("items",nlohmann::json({{"_id","duplicate"},{"name",replacement}}).dump()).success);
    }
    auto old=engine.FindMany("items",Condition::Leaf("name",Op::EQ,name),10,0);
    ASSERT_TRUE(old.success);
    EXPECT_TRUE(nlohmann::json::parse(old.data).empty());
}

TEST(RocksTuning, DropCollectionUsesBoundedAtomicRangeDeletesAndReopens) {
    TestTempDir temp("bounded_collection_drop");
    RocksSettings settings; settings.durability=DocumentDurability::Durable;
    settings.memory_budget_bytes=8*1024*1024;
    {
        DocEngine engine(temp.path().string(),{},settings);
        ASSERT_TRUE(engine.CreateCollection("drop").success);
        ASSERT_TRUE(engine.CreateCollection("drop_neighbor").success);
        ASSERT_TRUE(engine.InsertOne("drop_neighbor",R"({"_id":"keep","value":42})").success);
        IndexDefinition index; index.index_name="by_name"; index.fields={"name"}; index.type=IndexType::Unique;
        ASSERT_TRUE(engine.CreateIndex("drop",index).success);
        for (int batch=0;batch<80;++batch) {
            std::vector<std::string> values;
            for (int item=0;item<500;++item) {
                auto id=std::to_string(batch*500+item);
                values.push_back("{\"_id\":\""+id+"\",\"name\":\"name_"+id+"\"}");
            }
            auto inserted=engine.InsertMany("drop",values);
            ASSERT_TRUE(inserted.success)<<inserted.error_msg;
        }
        ASSERT_TRUE(engine.FlushStorage().success);
        const auto before=engine.GetRocksMetrics().wal_write_bytes;
        auto dropped=engine.DropCollection("drop");
        ASSERT_TRUE(dropped.success)<<dropped.error_msg;
        // Seven records (five ranges plus two exact metadata keys), not 80,000
        // point tombstones. This bound detects the old unbounded batch algorithm.
        const auto bytes=engine.GetRocksMetrics().wal_write_bytes-before;
        EXPECT_GT(bytes,0U);
        EXPECT_LT(bytes,16*1024U);
        EXPECT_FALSE(engine.CollectionExists("drop"));
        EXPECT_TRUE(engine.FindById("drop_neighbor","keep").success);
        EXPECT_FALSE(engine.DropCollection("drop").success);
        ASSERT_TRUE(engine.CreateCollection("drop").success);
        EXPECT_EQ(engine.Count("drop",Condition{}).data,"0");
        EXPECT_TRUE(engine.GetIndexes("drop").empty());
        ASSERT_TRUE(engine.CreateIndex("drop",index).success);
        auto reused=engine.InsertOne("drop",R"({"_id":"0","name":"name_0"})");
        ASSERT_TRUE(reused.success)<<reused.error_msg;
        EXPECT_EQ(engine.Count("drop",Condition{}).data,"1");
        ASSERT_TRUE(engine.PrepareShutdown().success);
    }
    DocEngine reopened(temp.path().string(),{},settings);
    EXPECT_EQ(reopened.Count("drop",Condition{}).data,"1");
    EXPECT_TRUE(reopened.FindById("drop","0").success);
    EXPECT_FALSE(reopened.FindById("drop","39999").success);
    EXPECT_TRUE(reopened.FindById("drop_neighbor","keep").success);
    auto lookup=reopened.FindMany("drop",Condition::Leaf("name",Op::EQ,"name_0"));
    ASSERT_TRUE(lookup.success)<<lookup.error_msg;
    EXPECT_EQ(nlohmann::json::parse(lookup.data).size(),1U);
}

TEST(RocksTuning, RangeDropWaitsForOpenTransactionBeforeDeletingCommittedData) {
    TestTempDir temp("range_drop_transaction_gate");
    DocEngine engine(temp.path().string());
    ASSERT_TRUE(engine.CreateCollection("items").success);
    auto transaction=engine.BeginTransaction();
    ASSERT_TRUE(transaction && transaction->IsValid());
    ASSERT_TRUE(engine.InsertOneTx(*transaction,"items",R"({"_id":"pending"})").success);
    std::atomic<bool> started=false,finished=false;
    DBResult dropped;
    std::thread dropper([&] {
        started=true;
        dropped=engine.DropCollection("items");
        finished=true;
    });
    while (!started.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(finished.load());
    auto committed=engine.CommitTransaction(*transaction);
    transaction.reset();
    dropper.join();
    ASSERT_TRUE(committed.success)<<committed.error_msg;
    ASSERT_TRUE(dropped.success)<<dropped.error_msg;
    ASSERT_TRUE(engine.CreateCollection("items").success);
    EXPECT_EQ(engine.Count("items",Condition{}).data,"0");
    EXPECT_FALSE(engine.FindById("items","pending").success);
}

TEST(RocksTuning, BsonInsertPreservesValidationAtomicityDefaultsAndIndexes) {
    TestTempDir temp("bson_insert_contract");
    RocksSettings settings; settings.durability=DocumentDurability::Durable;
    DocEngine engine(temp.path().string(),{},settings);
    const auto bson=[](const std::string& json) {
        auto encoded=DocumentCodec::EncodeJson(json);
        if (!encoded.success) throw std::runtime_error(encoded.error);
        return encoded.value.substr(DocumentCodec::kEnvelopeSize);
    };
    ASSERT_TRUE(engine.CreateCollection("items").success);
    IndexDefinition index; index.index_name="name"; index.fields={"name"}; index.type=IndexType::Unique;
    ASSERT_TRUE(engine.CreateIndex("items",index).success);
    ASSERT_TRUE(engine.InsertManyBson("items",{bson(R"({"_id":"a","name":"alpha","nested":{"x":null}})")}).success);
    EXPECT_NE(engine.FindById("items","a").data.find("alpha"),std::string::npos);
    auto lookup=engine.FindMany("items",Condition::Leaf("name",Op::EQ,"alpha"));
    ASSERT_TRUE(lookup.success)<<lookup.error_msg;
    EXPECT_EQ(nlohmann::json::parse(lookup.data).size(),1U);
    EXPECT_FALSE(engine.InsertManyBson("items",{bson(R"({"_id":"b","name":"beta"})"),
                                               bson(R"({"_id":"c","name":"alpha"})")}).success);
    EXPECT_FALSE(engine.FindById("items","b").success);
    EXPECT_FALSE(engine.InsertManyBson("items",{bson(R"({"_id":"b","name":"beta"})"),"invalid"}).success);
    EXPECT_FALSE(engine.FindById("items","b").success);
    EXPECT_FALSE(engine.InsertManyBson("items",{bson(R"({"_id":12})")}).success);
    EXPECT_FALSE(engine.InsertManyBson("items",{bson(R"({"_id":"same"})"),bson(R"({"_id":"same"})")}).success);
    EXPECT_EQ(engine.Count("items",Condition{}).data,"1");
    SchemaDefinition schema; schema.strict=true;
    SchemaField age; age.name="age"; age.type=FieldType::Int32; age.required=true; age.default_val="18";
    schema.fields.push_back(age);
    ASSERT_TRUE(engine.CreateCollection("defaults",schema).success);
    auto result=engine.InsertManyBson("defaults",{bson("{}")});
    ASSERT_TRUE(result.success)<<result.error_msg;
    auto id=nlohmann::json::parse(result.data)[0].get<std::string>();
    EXPECT_NE(engine.FindById("defaults",id).data.find("\"age\":18"),std::string::npos);
    EXPECT_FALSE(engine.InsertManyBson("defaults",{bson(R"({"_id":"bad","age":"wrong"})")}).success);
    ASSERT_TRUE(engine.CreateCollection("children").success);
    ForeignKeyDefinition fk; fk.fk_name="parent_name"; fk.local_field="parent";
    fk.ref_collection="items"; fk.ref_field="name";
    ASSERT_TRUE(engine.AddForeignKey("children",fk).success);
    EXPECT_TRUE(engine.InsertManyBson("children",{bson(R"({"_id":"ok","parent":"alpha"})")}).success);
    EXPECT_FALSE(engine.InsertManyBson("children",{bson(R"({"_id":"bad","parent":"absent"})")}).success);
    auto quoted=engine.InsertManyBson("items",{bson(R"({"_id":"quote\"slash\\","name":"quoted"})")});
    ASSERT_TRUE(quoted.success)<<quoted.error_msg;
    EXPECT_EQ(nlohmann::json::parse(quoted.data)[0].get<std::string>(),"quote\"slash\\");
    bson_t raw;
    bson_init(&raw);
    BSON_APPEND_UTF8(&raw,"_id","duplicate");
    BSON_APPEND_INT32(&raw,"value",1);
    BSON_APPEND_INT32(&raw,"value",2);
    const std::string duplicated(reinterpret_cast<const char*>(bson_get_data(&raw)),raw.len);
    EXPECT_FALSE(engine.InsertManyBson("items",{duplicated}).success);
    bson_destroy(&raw);
    bson_init(&raw);
    BSON_APPEND_UTF8(&raw,"_id","large_object");
    for (int i=0;i<20;++i) BSON_APPEND_INT32(&raw,("field"+std::to_string(i)).c_str(),i);
    BSON_APPEND_INT32(&raw,"field19",20);
    const std::string large_duplicate(reinterpret_cast<const char*>(bson_get_data(&raw)),raw.len);
    EXPECT_FALSE(engine.InsertManyBson("items",{large_duplicate}).success);
    bson_destroy(&raw);
    auto truncated=bson(R"({"_id":"truncated","value":1})");
    // Malformed/truncated payload must fail before any mutation.
    truncated.pop_back();
    EXPECT_FALSE(engine.InsertManyBson("items",{truncated}).success);
    ASSERT_TRUE(engine.PrepareShutdown().success);
}

TEST(RocksTuning, BudgetOptionsProfilesAndPrefixDomain) {
    {
        auto large=std::make_shared<RocksResources>(5120ULL*1024*1024,4096ULL*1024*1024);
        auto auto_options=MakeRocksOptions(RocksSettings{},large);
        EXPECT_EQ(auto_options.write_buffer_size,64U*1024*1024);
        EXPECT_LE(auto_options.write_buffer_size*auto_options.max_write_buffer_number,large->memtable_bytes);
        RocksSettings explicit_settings;
        explicit_settings.write_buffer_bytes=16*1024*1024;
        EXPECT_EQ(MakeRocksOptions(explicit_settings,large).write_buffer_size,16U*1024*1024);
    }
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
    auto* table=options.table_factory->GetOptions<rocksdb::BlockBasedTableOptions>();
    ASSERT_NE(table,nullptr);
    EXPECT_EQ(table->index_type,rocksdb::BlockBasedTableOptions::kTwoLevelIndexSearch);
    EXPECT_TRUE(table->partition_filters);
    EXPECT_EQ(table->metadata_block_size,4096U);
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
TEST(RocksTuning, PartitionedMetadataCompactsWithStrictSmallCache) {
    TestTempDir temp("rocks_partitioned_metadata");
    auto resources=std::make_shared<RocksResources>(4*1024*1024,1024*1024);
    RocksSettings settings;
    auto options=MakeRocksOptions(settings,resources);
    rocksdb::DB* raw=nullptr;
    ASSERT_TRUE(rocksdb::DB::Open(options,temp.path().string(),&raw).ok());
    std::unique_ptr<rocksdb::DB> db(raw);
    for (int start=0;start<40000;start+=500) {
        rocksdb::WriteBatch batch;
        for (int i=start;i<start+500;++i)
            batch.Put(std::to_string(i)+std::string(256,char('a'+i%26)),std::string(1024,char('A'+i%26)));
        auto status=db->Write(rocksdb::WriteOptions(),&batch);
        ASSERT_TRUE(status.ok())<<status.ToString();
    }
    rocksdb::FlushOptions flush; flush.wait=true;
    auto status=db->Flush(flush);
    ASSERT_TRUE(status.ok())<<status.ToString();
    status=db->CompactRange(rocksdb::CompactRangeOptions(),nullptr,nullptr);
    ASSERT_TRUE(status.ok())<<status.ToString();
    for (int i: {0,19999,39999}) {
        std::string value;
        status=db->Get(rocksdb::ReadOptions(),std::to_string(i)+std::string(256,char('a'+i%26)),&value);
        ASSERT_TRUE(status.ok())<<status.ToString();
        EXPECT_EQ(value,std::string(1024,char('A'+i%26)));
    }
    EXPECT_LE(resources->cache->GetUsage(),resources->cache_bytes);
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
