#include "TestTempDir.h"
#include "core/DocEngine.h"
#include <rocksdb/env.h>
#include <gtest/gtest.h>
#include <thread>
using namespace nexora::core;
using Json=nlohmann::json;
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
