#include "TestTempDir.h"

#include "core/DocEngine.h"
#include "query/Condition.h"
#include "query/UpdateSpec.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace {

    using nexora::core::DocEngine;
    using nexora::core::FieldType;
    using nexora::core::ForeignKeyDefinition;
    using nexora::core::IndexDefinition;
    using nexora::core::IndexType;
    using nexora::core::SchemaDefinition;
    using nexora::core::SchemaField;

    using nexora::query::Condition;
    using nexora::query::Op;
    using nexora::query::UpdateSpec;
    using nexora::query::UpdateValueType;
    using nexora::query::ValueType;

    TEST(DocEngineSmoke, OpensClosesAndReopensDatabase) {
        TestTempDir temp("nexora_doc_smoke");
        const auto database_path = temp.path() / "db";

        {
            DocEngine engine(database_path.string());
            ASSERT_TRUE(engine.IsHealthy());

            const auto result = engine.CreateCollection("users");
            ASSERT_TRUE(result.success) << result.error_msg;
        }

        {
            DocEngine engine(database_path.string());
            ASSERT_TRUE(engine.IsHealthy());
            EXPECT_TRUE(engine.CollectionExists("users"));
        }
    }

    TEST(DocEngineCrud, InsertFindUpdateQueryAndDelete) {
        TestTempDir temp("nexora_doc_crud");
        DocEngine engine((temp.path() / "db").string());

        ASSERT_TRUE(engine.CreateCollection("users").success);

        const auto insert = engine.InsertOne(
                "users",
                R"({"_id":"u1","name":"Ali","age":30})"
        );

        ASSERT_TRUE(insert.success) << insert.error_msg;
        EXPECT_EQ(insert.data, "u1");

        const auto found = engine.FindById("users", "u1");
        ASSERT_TRUE(found.success) << found.error_msg;
        EXPECT_NE(found.data.find("\"name\":\"Ali\""), std::string::npos);

        UpdateSpec update;
        update.Set("age", "31", UpdateValueType::Int64);
        update.Set("status", "active");

        const auto updated = engine.UpdateById("users", "u1", update);
        ASSERT_TRUE(updated.success) << updated.error_msg;
        EXPECT_EQ(updated.data, "1");

        const auto after_update = engine.FindById("users", "u1");
        ASSERT_TRUE(after_update.success);
        EXPECT_NE(after_update.data.find("\"age\":31"), std::string::npos);
        EXPECT_NE(after_update.data.find("\"status\":\"active\""), std::string::npos);

        const auto condition =
                Condition::Leaf("age", Op::GTE, "31", ValueType::Int64);

        const auto query = engine.FindMany("users", condition);
        ASSERT_TRUE(query.success);
        EXPECT_NE(query.data.find("\"_id\":\"u1\""), std::string::npos);

        const auto removed = engine.DeleteById("users", "u1");
        ASSERT_TRUE(removed.success);
        EXPECT_EQ(removed.data, "1");

        EXPECT_FALSE(engine.FindById("users", "u1").success);
    }

    TEST(DocEngineSchema, RejectsMissingRequiredField) {
        TestTempDir temp("nexora_doc_schema");
        DocEngine engine((temp.path() / "db").string());

        SchemaDefinition schema;
        schema.fields.push_back(SchemaField{
                .name = "name",
                .type = FieldType::String,
                .required = true,
                .unique = false,
                .default_val = std::nullopt
        });

        const auto created = engine.CreateCollection("users", schema);
        ASSERT_TRUE(created.success) << created.error_msg;

        const auto invalid =
                engine.InsertOne("users", R"({"_id":"u1","age":20})");

        EXPECT_FALSE(invalid.success);

        const auto valid =
                engine.InsertOne("users", R"({"_id":"u2","name":"Sara"})");

        EXPECT_TRUE(valid.success) << valid.error_msg;
    }

    TEST(DocEngineIndex, CreatesListsAndDropsIndex) {
        TestTempDir temp("nexora_doc_index");
        DocEngine engine((temp.path() / "db").string());

        ASSERT_TRUE(engine.CreateCollection("users").success);
        ASSERT_TRUE(
                engine.InsertOne(
                        "users",
                        R"({"_id":"u1","email":"ali@example.com"})"
                ).success
        );

        IndexDefinition index;
        index.index_name = "idx_users_email";
        index.fields = {"email"};
        index.type = IndexType::SingleField;

        const auto created = engine.CreateIndex("users", index);
        ASSERT_TRUE(created.success) << created.error_msg;

        const auto indexes = engine.GetIndexes("users");
        ASSERT_EQ(indexes.size(), 1U);
        EXPECT_EQ(indexes.front().index_name, "idx_users_email");
        ASSERT_EQ(indexes.front().fields.size(), 1U);
        EXPECT_EQ(indexes.front().fields.front(), "email");

        const auto removed =
                engine.DropIndex("users", "idx_users_email");

        ASSERT_TRUE(removed.success) << removed.error_msg;
        EXPECT_TRUE(engine.GetIndexes("users").empty());
    }

    TEST(DocEngineForeignKey, AcceptsValidAndRejectsInvalidReference) {
        TestTempDir temp("nexora_doc_fk");
        DocEngine engine((temp.path() / "db").string());

        ASSERT_TRUE(engine.CreateCollection("users").success);
        ASSERT_TRUE(engine.CreateCollection("posts").success);

        ASSERT_TRUE(
                engine.InsertOne(
                        "users",
                        R"({"_id":"u1","name":"Ali"})"
                ).success
        );

        ForeignKeyDefinition foreign_key;
        foreign_key.fk_name = "fk_posts_author";
        foreign_key.local_field = "author_id";
        foreign_key.ref_collection = "users";
        foreign_key.ref_field = "_id";

        const auto added =
                engine.AddForeignKey("posts", foreign_key);

        ASSERT_TRUE(added.success) << added.error_msg;

        const auto valid = engine.InsertOne(
                "posts",
                R"({"_id":"p1","author_id":"u1","title":"valid"})"
        );

        EXPECT_TRUE(valid.success) << valid.error_msg;

        const auto invalid = engine.InsertOne(
                "posts",
                R"({"_id":"p2","author_id":"missing","title":"invalid"})"
        );

        EXPECT_FALSE(invalid.success);
    }

    TEST(DocEngineTransaction, CommitPersistsAndRollbackDiscards) {
        TestTempDir temp("nexora_doc_transaction");
        DocEngine engine((temp.path() / "db").string());

        ASSERT_TRUE(engine.CreateCollection("users").success);

        {
            auto transaction = engine.BeginTransaction();
            ASSERT_NE(transaction, nullptr);

            const auto inserted = engine.InsertOneTx(
                    *transaction,
                    "users",
                    R"({"_id":"committed","name":"Committed"})"
            );

            ASSERT_TRUE(inserted.success) << inserted.error_msg;

            const auto committed = engine.CommitTransaction(*transaction);
            ASSERT_TRUE(committed.success) << committed.error_msg;
        }

        EXPECT_TRUE(engine.FindById("users", "committed").success);

        {
            auto transaction = engine.BeginTransaction();
            ASSERT_NE(transaction, nullptr);

            const auto inserted = engine.InsertOneTx(
                    *transaction,
                    "users",
                    R"({"_id":"rolled_back","name":"Rolled Back"})"
            );

            ASSERT_TRUE(inserted.success) << inserted.error_msg;

            const auto rolled_back =
                    engine.RollbackTransaction(*transaction);

            ASSERT_TRUE(rolled_back.success) << rolled_back.error_msg;
        }

        EXPECT_FALSE(engine.FindById("users", "rolled_back").success);
    }

} // namespace