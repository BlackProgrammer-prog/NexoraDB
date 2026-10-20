#include "TestTempDir.h"
#include "core/DocEngine.h"
#include "core/IndexCodec.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <rocksdb/db.h>
using namespace nexora::core;
namespace {
void rawPut(const std::filesystem::path& path,const std::string& key,const std::string& value) {
    rocksdb::DB* raw=nullptr; rocksdb::Options options;
    ASSERT_TRUE(rocksdb::DB::Open(options,path.string(),&raw).ok());
    std::unique_ptr<rocksdb::DB> db(raw); ASSERT_TRUE(db->Put({},key,value).ok());
}
}
TEST(Maintenance, CheckpointIsIndependentAndNeverOverwrites) {
    TestTempDir temp("checkpoint"); DocEngine source((temp.path()/"source").string());
    source.PauseDocumentMigration();
    ASSERT_TRUE(source.CreateCollection("items").success);
    ASSERT_TRUE(source.InsertOne("items",R"({"_id":"a","v":1})").success);
    ASSERT_TRUE(source.CreateCheckpoint((temp.path()/"backup").string()).success);
    EXPECT_FALSE(source.CreateCheckpoint((temp.path()/"backup").string()).success);
    EXPECT_FALSE(source.CreateCheckpoint((temp.path()/"source"/"nested").string()).success);
    ASSERT_TRUE(source.InsertOne("items",R"({"_id":"b","v":2})").success);
    DocEngine backup((temp.path()/"backup").string());
    EXPECT_TRUE(backup.FindById("items","a").success);
    EXPECT_FALSE(backup.FindById("items","b").success);
    EXPECT_TRUE(backup.IntegrityCheck().success);
}
TEST(Maintenance, DetectsMissingAndStaleIndexesThenRebuilds) {
    TestTempDir temp("integrity"); std::string id;
    {
        DocEngine db(temp.path().string()); ASSERT_TRUE(db.CreateCollection("items").success);
        ASSERT_TRUE(db.InsertOne("items",R"({"_id":"a","v":1})").success);
        IndexDefinition index; index.index_name="v"; index.fields={"v"}; ASSERT_TRUE(db.CreateIndex("items",index).success);
        id=db.GetIndexes("items")[0].index_id; ASSERT_TRUE(db.IntegrityCheck().success);
    }
    rawPut(temp.path(),indexv2::IndexPrefix("items",id)+"Ebad","missing");
    {
        DocEngine db(temp.path().string()); EXPECT_FALSE(db.IntegrityCheck().success);
        ASSERT_TRUE(db.RebuildIndexes("items").success); EXPECT_TRUE(db.IntegrityCheck().success);
    }
    rawPut(temp.path(),"data:items:a","NXD9bad");
    DocEngine db(temp.path().string()); EXPECT_FALSE(db.IntegrityCheck().success); EXPECT_FALSE(db.IsHealthy());
}
TEST(Maintenance, DryRunUpgradeFenceAndRollbackCheckpoint) {
    TestTempDir temp("migration"); const auto source=temp.path()/"source",old=temp.path()/"old";
    {
        DocEngine db(source.string()); ASSERT_TRUE(db.CreateCollection("items").success);
        ASSERT_TRUE(db.InsertOne("items",R"({"_id":"a","v":1})").success);
    }
    rawPut(source,"data:items:legacy",R"({"_id":"legacy","v":2})");
    const auto inspected=DocEngine::InspectStore(source.string()); ASSERT_TRUE(inspected.success)<<inspected.error_msg;
    EXPECT_EQ(nlohmann::json::parse(inspected.data)["legacy_documents"],1);
    {
        DocEngine db(source.string()); ASSERT_TRUE(db.CreateCheckpoint(old.string()).success);
        ASSERT_TRUE(db.MigrateLegacyDocuments().success);
        ASSERT_TRUE(db.UpgradeFormatManifest().success);
        EXPECT_TRUE(nlohmann::json::parse(db.FormatVersions().data)["installed"]);
    }
    {
        rocksdb::DB* raw=nullptr; rocksdb::Options options;
        ASSERT_TRUE(rocksdb::DB::OpenForReadOnly(options,source.string(),&raw).ok());
        std::unique_ptr<rocksdb::DB> db(raw); std::string marker;
        ASSERT_TRUE(db->Get({},"meta:format:documents",&marker).ok());
        EXPECT_NE(marker,"1"); // Exact pre-phase-8 constructor reader fence.
    }
    { DocEngine rollback(old.string()); EXPECT_FALSE(nlohmann::json::parse(rollback.FormatVersions().data)["installed"]); EXPECT_TRUE(rollback.FindById("items","legacy").success); }
    rawPut(source,"meta:format:components",R"({"manifest":99})");
    EXPECT_THROW(DocEngine unsupported(source.string()),std::runtime_error);
}
TEST(Maintenance, RejectsEveryUnknownComponentVersion) {
    TestTempDir temp("future_versions"); nlohmann::json versions;
    {
        DocEngine db(temp.path().string()); ASSERT_TRUE(db.MigrateLegacyDocuments().success);
        const auto upgraded=db.UpgradeFormatManifest(); ASSERT_TRUE(upgraded.success)<<upgraded.error_msg;
        versions=nlohmann::json::parse(upgraded.data);
    }
    for(auto it=versions.begin();it!=versions.end();++it) {
        auto future=versions; future[it.key()]=it.value().get<unsigned>()+1;
        rawPut(temp.path(),"meta:format:components",future.dump());
        EXPECT_THROW(DocEngine rejected(temp.path().string()),std::runtime_error)<<it.key();
    }
}
