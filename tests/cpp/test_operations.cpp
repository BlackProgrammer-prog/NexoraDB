#include "TestTempDir.h"
#include "core/DocEngine.h"
#include "core/DocumentCodec.h"
#include <rocksdb/env.h>
#include <gtest/gtest.h>
#include <thread>
using namespace nexora::core;
using Json=nlohmann::json;
TEST(Operations, ExclusiveIngestWaitsForTransactionsAndRollsBackInjectedFaults) {
    TestTempDir temp("exclusive_ingest_gate");
    RocksSettings settings;settings.durability=DocumentDurability::Durable;
    DocEngine engine(temp.path().string(),{},settings);
    ASSERT_TRUE(engine.CreateCollection("items").success);
    auto first=DocumentCodec::EncodeJson(R"({"_id":"one","value":1})");
    auto second=DocumentCodec::EncodeJson(R"({"_id":"two","value":2})");
    std::string one=first.value.substr(DocumentCodec::kEnvelopeSize),two=second.value.substr(DocumentCodec::kEnvelopeSize);
    auto tx=engine.BeginTransaction();ASSERT_TRUE(tx);
    std::atomic<bool> entered{false},done{false};DBResult result;
    std::thread writer([&] {entered=true;result=engine.InsertManyBsonIngest("items",{one});done=true;});
    while(!entered.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(done.load());
    EXPECT_TRUE(engine.RollbackTransaction(*tx).success);
    writer.join();ASSERT_TRUE(result.success)<<result.error_msg;
    EXPECT_EQ(result.data,"1");
    const auto before=engine.GetRocksMetrics().wal_write_bytes;
    for(const auto point:{MutationFaultPoint::AfterDocument,MutationFaultPoint::AfterIndexes,MutationFaultPoint::AfterCounter}) {
        engine.SetMutationFaultPointForTesting(point);
        EXPECT_FALSE(engine.InsertManyBsonIngest("items",{two}).success);
        EXPECT_EQ(engine.GetCollectionSize("items"),1U);
        EXPECT_FALSE(engine.FindById("items","two").success);
        EXPECT_EQ(engine.GetRocksMetrics().wal_write_bytes,before);
    }
    engine.SetMutationFaultPointForTesting(MutationFaultPoint::None);
    EXPECT_TRUE(engine.InsertManyBsonIngest("items",{two}).success);
    EXPECT_EQ(engine.GetCollectionSize("items"),2U);
}
TEST(Operations, ConcurrentDurableBatchesNeverExposeHalfToSelect) {
    TestTempDir temp("durable_batch_select");
    RocksSettings settings;settings.durability=DocumentDurability::Durable;
    DocEngine engine(temp.path().string(),{},settings);
    ASSERT_TRUE(engine.CreateCollection("items").success);
    std::atomic<int> done{0},errors{0};
    std::vector<std::thread> writers;
    for(int worker=0;worker<4;++worker) writers.emplace_back([&,worker] {
        for(int i=0;i<25;++i) {
            std::string id=std::to_string(worker)+"_"+std::to_string(i);
            if(!engine.InsertMany("items",{Json({{"_id",id+"a"}}).dump(),Json({{"_id",id+"b"}}).dump()}).success) ++errors;
        }
        ++done;
    });
    do {
        auto result=engine.FindMany("items",{});
        if(!result.success || Json::parse(result.data).size()%2) ++errors;
    } while(done.load()!=4);
    for(auto& writer:writers) writer.join();
    EXPECT_EQ(errors,0);EXPECT_EQ(engine.GetCollectionSize("items"),200U);
    const auto path=Json::parse(engine.OperationMetrics())["write_path"];
    EXPECT_TRUE(path["durable_ack"].get<bool>());
    EXPECT_FALSE(path["unordered_write"].get<bool>());
    EXPECT_GT(path["wal_syncs"].get<uint64_t>(),0U);
}
TEST(Operations, ExclusiveIngestKeepsSnapshotSelectAtomic) {
    TestTempDir temp("exclusive_ingest_snapshot");
    RocksSettings settings;settings.durability=DocumentDurability::Durable;
    DocEngine engine(temp.path().string(),{},settings);
    ASSERT_TRUE(engine.CreateCollection("items").success);
    std::atomic<bool> done{false};std::atomic<int> errors{0};
    std::thread writer([&] {
        for(int i=0;i<30;++i) {
            const auto id=std::to_string(i);
            auto a=DocumentCodec::EncodeJson(Json({{"_id",id+"a"}}).dump());
            auto b=DocumentCodec::EncodeJson(Json({{"_id",id+"b"}}).dump());
            const auto one=a.value.substr(DocumentCodec::kEnvelopeSize),two=b.value.substr(DocumentCodec::kEnvelopeSize);
            if(!engine.InsertManyBsonIngest("items",{one,two}).success) ++errors;
        }
        done=true;
    });
    do {
        auto page=engine.FindPage("items",{},1000);
        if(!page.success || Json::parse(page.data).size()%2) ++errors;
    } while(!done.load());
    writer.join();EXPECT_EQ(errors,0);EXPECT_EQ(engine.GetCollectionSize("items"),60U);
}
namespace {
class FailureEnv:public rocksdb::EnvWrapper {
public:
    std::atomic<int> failure{0};
    FailureEnv():EnvWrapper(rocksdb::Env::Default()) {}
    rocksdb::Status injected() { return failure==1 ? rocksdb::Status::NoSpace("injected ENOSPC") : rocksdb::Status::IOError("injected EACCES: Permission denied"); }
    struct File:rocksdb::WritableFileWrapper {
        std::unique_ptr<rocksdb::WritableFile> owned; FailureEnv& env;
        File(std::unique_ptr<rocksdb::WritableFile> file,FailureEnv& value):WritableFileWrapper(file.get()),owned(std::move(file)),env(value) {}
        rocksdb::Status Append(const rocksdb::Slice& data) override {
            return env.failure ? env.injected() : owned->Append(data);
        }
        rocksdb::Status Append(const rocksdb::Slice& data,const rocksdb::DataVerificationInfo& info) override {
            return env.failure ? env.injected() : owned->Append(data,info);
        }
    };
    rocksdb::Status NewWritableFile(const std::string& path,std::unique_ptr<rocksdb::WritableFile>* file,const rocksdb::EnvOptions& options) override {
        auto status=target()->NewWritableFile(path,file,options);
        if(status.ok() && path.ends_with(".log")) *file=std::make_unique<File>(std::move(*file),*this);
        return status;
    }
};
}
TEST(Operations, HistogramsErrorRatesSlowPlanAndConcurrency) {
    TestTempDir temp("operations"); DocEngine engine(temp.path().string()); engine.PauseDocumentMigration();
    ASSERT_TRUE(engine.IsHealthy()); ASSERT_TRUE(engine.CreateCollection("items").success);
    ASSERT_TRUE(engine.InsertOne("items",R"({"_id":"a","secret":"must-not-log"})").success);
    engine.ConfigureSlowQueries(1,2);
    for(int i=0;i<4;++i) ASSERT_TRUE(engine.FindMany("items",{}).success);
    std::vector<std::thread> workers;
    for(int i=0;i<4;++i) workers.emplace_back([&]{for(int n=0;n<50;++n) (void)engine.FindById("items","a");});
    for(auto& worker:workers) worker.join();
    EXPECT_FALSE(engine.FindById("items","absent").success);
    EXPECT_FALSE(engine.FindMany("absent",{}).success);
    const auto metrics=Json::parse(engine.OperationMetrics());
    const auto point=metrics["operations"]["point_read"];
    EXPECT_EQ(point["calls_total"],201); EXPECT_EQ(point["not_found_total"],1); EXPECT_EQ(point["errors_total"],0);
    EXPECT_EQ(point["buckets"].back()["count"],201);
    uint64_t previous=0; for(const auto& bucket:point["buckets"]) {EXPECT_GE(bucket["count"].get<uint64_t>(),previous);previous=bucket["count"];}
    EXPECT_EQ(metrics["operations"]["query"]["errors_total"],1);
    const auto events=engine.DrainSlowQueries(); EXPECT_EQ(events.find("must-not-log"),std::string::npos);
    ASSERT_LE(Json::parse(events).size(),2U); EXPECT_GT(metrics["slow_events_dropped"].get<uint64_t>(),0U);
    for(const auto& event:Json::parse(events)) EXPECT_TRUE(event["explain"].contains("scan_type"));
    EXPECT_EQ(engine.DrainSlowQueries(),"[]");
    ASSERT_TRUE(engine.PrepareShutdown().success); EXPECT_FALSE(engine.IsHealthy());
    EXPECT_TRUE(Json::parse(engine.HealthReport())["quiescing"]);
}
TEST(Operations, DiskFullAndPermissionWalErrorsFailClosedWithoutPartialInsert) {
    for(int failure:{1,2}) {
        TestTempDir temp("io_failure"); FailureEnv env; RocksSettings settings; settings.env_for_testing=&env;
        settings.durability=DocumentDurability::Durable;
        DocEngine engine(temp.path().string(),{},settings); engine.PauseDocumentMigration();
        ASSERT_TRUE(engine.CreateCollection("items").success); ASSERT_TRUE(engine.IsHealthy());
        env.failure=failure;
        const auto inserted=engine.InsertOne("items",R"({"_id":"a","v":1})");
        EXPECT_FALSE(inserted.success); EXPECT_FALSE(engine.IsHealthy());
        EXPECT_TRUE(Json::parse(engine.HealthReport())["io_error_latched"]);
        EXPECT_FALSE(engine.PrepareShutdown().success);
        env.failure=0;
        EXPECT_FALSE(engine.FindById("items","a").success);
    }
}
TEST(Operations, ExclusiveIngestWalFailureDoesNotPublishPartialBatch) {
    for(int failure:{1,2}) {
        TestTempDir temp("bulk_io_failure");FailureEnv env;RocksSettings settings;
        settings.env_for_testing=&env;settings.durability=DocumentDurability::Durable;
        DocEngine engine(temp.path().string(),{},settings);engine.PauseDocumentMigration();
        ASSERT_TRUE(engine.CreateCollection("items").success);
        auto encoded=DocumentCodec::EncodeJson(R"({"_id":"one"})");
        const auto bson=encoded.value.substr(DocumentCodec::kEnvelopeSize);
        env.failure=failure;
        const auto result=engine.InsertManyBsonIngest("items",{bson});
        EXPECT_FALSE(result.success);EXPECT_EQ(result.error_code,"io_error");
        env.failure=0;
        EXPECT_FALSE(engine.FindById("items","one").success);
        EXPECT_EQ(engine.GetCollectionSize("items"),0U);
        EXPECT_TRUE(Json::parse(engine.HealthReport())["io_error_latched"]);
    }
}
