#include "TestTempDir.h"
#include "RecoveryTestSupport.h"
#include "core/IndexCodec.h"
#include <rocksdb/db.h>
#include <gtest/gtest.h>

using namespace recovery_test;
namespace {
const std::vector<std::string> modes = {"insert", "json", "bson", "ingest", "update", "delete", "update_many", "delete_many", "tx"};
using Case = std::tuple<std::string, int>;
std::vector<Case> stagingCases() {
    std::vector<Case> cases;
    for (const auto& mode : modes) for (int stage : {1, 2, 3}) {
        if ((mode == "update" || mode == "update_many") && stage == 3) continue;
        cases.emplace_back(mode, stage);
    }
    return cases;
}
class FailureRecovery : public testing::TestWithParam<Case> {};

TEST_P(FailureRecovery, WalErrorHasNoSuccessAckAndRecoversWholeState) {
    const auto [mode, failure] = GetParam();
    TestTempDir temp("cor007_wal"); FaultEnv env;
    {
        DocEngine db(temp.path().string(), {}, settings(&env)); seed(db);
        env.failure = failure;
        const auto result = mutate(db, mode);
        EXPECT_FALSE(result.success) << mode;
        EXPECT_EQ(result.error_code, "io_error") << result.error_msg;
        EXPECT_GT(env.hits, 0); // Prove the intended storage boundary was reached.
        EXPECT_FALSE(db.IsHealthy());
        EXPECT_FALSE(db.PrepareShutdown().success);
        env.failure = 0;
        if (failure != 3) { EXPECT_NO_THROW(verify(db, expected(mode, false))); }
    }
    DocEngine reopened(temp.path().string(), {}, settings()); reopened.PauseDocumentMigration();
    const auto rows = snapshot(reopened);
    if (failure != 3) EXPECT_EQ(rows, expected(mode, false));
    else EXPECT_TRUE(rows == expected(mode, false) || rows == expected(mode, true));
    EXPECT_NO_THROW(verify(reopened, rows));
}
INSTANTIATE_TEST_SUITE_P(COR007, FailureRecovery,
    testing::Combine(testing::ValuesIn(modes), testing::Values(1, 2, 3)),
    [](const testing::TestParamInfo<Case>& info) {
        return std::get<0>(info.param) + "_fault" + std::to_string(std::get<1>(info.param));
    });

class StagingRecovery : public testing::TestWithParam<Case> {};
TEST_P(StagingRecovery, RejectsMutationAndReopensWithoutPartialState) {
    const auto [mode, stage] = GetParam();
    TestTempDir temp("cor007_stage");
    {
        DocEngine db(temp.path().string(), {}, settings()); seed(db);
        db.SetMutationFaultPointForTesting(static_cast<MutationFaultPoint>(stage));
        const auto result = mutate(db, mode);
        EXPECT_FALSE(result.success);
        EXPECT_NE(result.error_msg.find("Injected mutation fault"), std::string::npos);
        EXPECT_NO_THROW(verify(db, expected(mode, false)));
    }
    DocEngine reopened(temp.path().string(), {}, settings()); reopened.PauseDocumentMigration();
    EXPECT_NO_THROW(verify(reopened, expected(mode, false)));
}
INSTANTIATE_TEST_SUITE_P(COR007, StagingRecovery,
    testing::ValuesIn(stagingCases()),
    [](const testing::TestParamInfo<Case>& info) {
        return std::get<0>(info.param) + "_stage" + std::to_string(std::get<1>(info.param));
    });

TEST(FailureRecoveryTransaction, FailedSavepointPreservesEarlierStagingWhenTransactionCommits) {
    for (const auto& mode : {"insert", "update", "delete"}) for (int stage : {1, 2, 3}) {
        if (std::string(mode) == "update" && stage == 3) continue;
        SCOPED_TRACE(std::string(mode) + std::to_string(stage));
        TestTempDir temp("cor007_savepoint");
        const auto rows = expected("insert", true);
        {
            DocEngine db(temp.path().string(), {}, settings()); seed(db);
            auto tx = db.BeginTransaction(); ASSERT_TRUE(tx);
            checked(db.InsertOneTx(*tx, "items", row("c", "new-c").dump()));
            db.SetMutationFaultPointForTesting(static_cast<MutationFaultPoint>(stage));
            nexora::query::UpdateSpec update; update.Set("key", "new-a");
            const auto failed = std::string(mode) == "insert" ? db.InsertOneTx(*tx, "items", row("d", "new-d").dump()) :
                std::string(mode) == "update" ? db.UpdateByIdTx(*tx, "items", "a", update) : db.DeleteByIdTx(*tx, "items", "a");
            ASSERT_FALSE(failed.success);
            checked(db.CommitTransaction(*tx));
            EXPECT_NO_THROW(verify(db, rows));
        }
        DocEngine db(temp.path().string(), {}, settings()); db.PauseDocumentMigration();
        EXPECT_NO_THROW(verify(db, rows));
    }
}

TEST(FailureRecoveryTransaction, UniqueAndForeignKeyCollectionsRecoverTogetherAfterWalError) {
    for (int failure : {1, 2, 3}) {
        TestTempDir temp("cor007_fk_io"); FaultEnv env;
        {
            DocEngine db(temp.path().string(), {}, settings(&env)); db.PauseDocumentMigration();
            checked(db.CreateCollection("parents")); checked(db.CreateCollection("children"));
            IndexDefinition index; index.index_name = "email"; index.fields = {"email"}; index.type = IndexType::Unique;
            checked(db.CreateIndex("parents", index));
            ForeignKeyDefinition fk; fk.fk_name = "parent"; fk.local_field = "email";
            fk.ref_collection = "parents"; fk.ref_field = "email";
            checked(db.AddForeignKey("children", fk));
            auto tx = db.BeginTransaction(); ASSERT_TRUE(tx);
            checked(db.InsertOneTx(*tx, "parents", R"({"_id":"p","email":"p@example.org"})"));
            checked(db.InsertOneTx(*tx, "children", R"({"_id":"c","email":"p@example.org"})"));
            env.failure = failure;
            EXPECT_FALSE(db.CommitTransaction(*tx).success); EXPECT_FALSE(tx->IsValid());
            EXPECT_GT(env.hits, 0); EXPECT_FALSE(db.PrepareShutdown().success);
            env.failure = 0;
        }
        DocEngine db(temp.path().string(), {}, settings()); db.PauseDocumentMigration();
        const auto parent = db.FindById("parents", "p"), child = db.FindById("children", "c");
        EXPECT_EQ(parent.success, child.success);
        if (failure != 3) { EXPECT_FALSE(parent.success); }
        EXPECT_EQ(db.GetCollectionSize("parents"), parent.success ? 1 : 0);
        EXPECT_EQ(db.GetCollectionSize("children"), child.success ? 1 : 0);
        EXPECT_TRUE(db.IntegrityCheck().success);
    }
}

void rawPut(const std::filesystem::path& path, const std::string& key, const std::string& value) {
    rocksdb::DB* raw = nullptr;
    const auto status = rocksdb::DB::Open({}, path.string(), &raw);
    ASSERT_TRUE(status.ok()) << status.ToString();
    std::unique_ptr<rocksdb::DB> db(raw);
    ASSERT_TRUE(db->Put({}, key, value).ok());
}
TEST(FailureRecoveryCorruption, MalformedDocumentsFailPointScanPageIndexAndMutation) {
    for (const auto& bytes : {std::string("NXD9bad"), std::string("NXD\x01\x05\0\0\0\0", 9), std::string("{broken")}) {
        TestTempDir temp("cor007_document_corrupt");
        { DocEngine db(temp.path().string(), {}, settings()); seed(db); }
        rawPut(temp.path(), "data:items:a", bytes);
        DocEngine db(temp.path().string(), {}, settings()); db.PauseDocumentMigration();
        EXPECT_FALSE(db.FindById("items", "a").success);
        EXPECT_FALSE(db.FindMany("items", {}).success);
        EXPECT_FALSE(db.FindMany("items", eq("old-a")).success);
        EXPECT_FALSE(db.FindPage("items", {}, 100).success);
        EXPECT_FALSE(db.UpdateById("items", "a", nexora::query::UpdateSpec{}.Set("key", "new-a")).success);
        EXPECT_FALSE(db.DeleteById("items", "a").success);
        EXPECT_FALSE(db.IntegrityCheck().success);
        EXPECT_FALSE(db.IsHealthy());
    }
}
TEST(FailureRecoveryCorruption, DanglingIndexCannotSilentlyReturnEmptyResults) {
    TestTempDir temp("cor007_dangling"); std::string index_key;
    {
        DocEngine db(temp.path().string(), {}, settings()); seed(db);
        const auto id = db.GetIndexes("items")[0].index_id;
        auto tuple = indexv2::EncodeValues({nexora::query::FieldValue{"old-a", nexora::query::ValueType::String, true}});
        ASSERT_TRUE(tuple);
        index_key = indexv2::EntryKey("items", id, *tuple, "a");
    }
    rawPut(temp.path(), index_key, "missing-document");
    DocEngine db(temp.path().string(), {}, settings()); db.PauseDocumentMigration();
    const auto result = db.FindMany("items", eq("old-a"));
    EXPECT_FALSE(result.success); EXPECT_EQ(result.error_code, "corruption");
    EXPECT_FALSE(db.Count("items", eq("old-a")).success);
    EXPECT_FALSE(db.Exists("items", eq("old-a")).success);
    EXPECT_FALSE(db.IsHealthy());
}
TEST(FailureRecoveryCorruption, CounterNeverBecomesSuccessfulZeroOrNumericPrefix) {
    for (const auto& value : {"bad", "2garbage", "-1", "9223372036854775808", "", "+2", " 2"}) {
        TestTempDir temp("cor007_counter_corrupt");
        { DocEngine db(temp.path().string(), {}, settings()); seed(db); }
        rawPut(temp.path(), "seq:items", value);
        DocEngine db(temp.path().string(), {}, settings()); db.PauseDocumentMigration();
        const auto count = db.Count("items", {});
        EXPECT_FALSE(count.success) << value;
        EXPECT_EQ(count.error_code, "corruption");
        EXPECT_EQ(db.GetCollectionSize("items"), -1);
        EXPECT_FALSE(db.InsertOne("items", row("c", "new-c").dump()).success);
        EXPECT_EQ(snapshot(db), expected("insert", false));
        EXPECT_FALSE(db.IntegrityCheck(false).success);
        EXPECT_FALSE(db.IsHealthy());
    }
}
TEST(FailureRecoveryCorruption, IntegrityChecksCounterAgainstDocumentsEvenWithoutIndexes) {
    TestTempDir temp("cor007_counter_mismatch");
    { DocEngine db(temp.path().string(), {}, settings()); seed(db); }
    rawPut(temp.path(), "seq:items", "1");
    DocEngine db(temp.path().string(), {}, settings()); db.PauseDocumentMigration();
    EXPECT_FALSE(db.DeleteMany("items", {}).success);
    EXPECT_FALSE(db.IntegrityCheck(false).success);
    EXPECT_EQ(snapshot(db), expected("insert", false));
}
TEST(FailureRecoveryCorruption, MissingCounterIsNotEmptyCollection) {
    TestTempDir temp("cor007_counter_missing");
    { DocEngine db(temp.path().string(), {}, settings()); seed(db); }
    {
        rocksdb::DB* raw = nullptr;
        ASSERT_TRUE(rocksdb::DB::Open({}, temp.path().string(), &raw).ok());
        std::unique_ptr<rocksdb::DB> db(raw);
        ASSERT_TRUE(db->Delete({}, "seq:items").ok());
    }
    DocEngine db(temp.path().string(), {}, settings()); db.PauseDocumentMigration();
    EXPECT_FALSE(db.Count("items", {}).success);
    EXPECT_FALSE(db.InsertOne("items", row("c", "new-c").dump()).success);
    EXPECT_FALSE(db.IntegrityCheck().success);
    EXPECT_EQ(snapshot(db), expected("insert", false));
}
} // namespace
