#include "TestTempDir.h"

#include "core/DocEngine.h"
#include "core/IndexCodec.h"
#include "query/Condition.h"
#include "query/UpdateSpec.h"

#include <gtest/gtest.h>

#include <atomic>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

    using nexora::core::DocEngine;
    using nexora::core::BulkWriteMode;
    using nexora::core::BulkWriteOptions;
    using nexora::core::FieldType;
    using nexora::core::ForeignKeyDefinition;
    using nexora::core::IndexDefinition;
    using nexora::core::IndexType;
    using nexora::core::MutationFaultPoint;
    using nexora::core::SchemaDefinition;
    using nexora::core::SchemaField;
    using nexora::core::TransactionSettings;

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

    TEST(DocEngineMutation,
         KeepsDocumentIndexesAndCounterConsistent) {
        TestTempDir temp("nexora_doc_atomic_mutation");
        DocEngine engine((temp.path() / "db").string());

        ASSERT_TRUE(engine.CreateCollection("users").success);
        ASSERT_TRUE(engine.CreateCollection("posts").success);

        IndexDefinition index;
        index.index_name = "idx_users_email";
        index.fields = {"email"};
        index.type = IndexType::SingleField;

        const auto index_result =
                engine.CreateIndex("users", index);
        ASSERT_TRUE(index_result.success)
                << index_result.error_msg;

        ForeignKeyDefinition foreign_key;
        foreign_key.fk_name = "fk_posts_user_email";
        foreign_key.local_field = "user_email";
        foreign_key.ref_collection = "users";
        foreign_key.ref_field = "email";

        const auto foreign_key_result =
                engine.AddForeignKey("posts", foreign_key);
        ASSERT_TRUE(foreign_key_result.success)
                << foreign_key_result.error_msg;

        ASSERT_TRUE(engine.InsertOne(
                "users",
                R"({"_id":"u1","email":"old@example.com"})"
        ).success);

        const auto initial_count =
                engine.Count("users", Condition{});
        ASSERT_TRUE(initial_count.success)
                << initial_count.error_msg;
        EXPECT_EQ(initial_count.data, "1");

        const auto duplicate = engine.InsertOne(
                "users",
                R"({"_id":"u1","email":"ghost@example.com","name":"Ali"})"
        );
        EXPECT_FALSE(duplicate.success);
        EXPECT_NE(duplicate.error_msg.find("Duplicate document _id"),
                  std::string::npos);

        const auto count_after_duplicate =
                engine.Count("users", Condition{});
        ASSERT_TRUE(count_after_duplicate.success)
                << count_after_duplicate.error_msg;
        EXPECT_EQ(count_after_duplicate.data, "1");

        // A rejected duplicate must not leave an index entry for its payload.
        EXPECT_FALSE(engine.InsertOne(
                "posts",
                R"({"_id":"p_ghost","user_email":"ghost@example.com"})"
        ).success);

        UpdateSpec update;
        update.Set("email", "new@example.com");

        const auto update_result =
                engine.UpdateById("users", "u1", update);
        ASSERT_TRUE(update_result.success)
                << update_result.error_msg;

        const auto old_index_reference = engine.InsertOne(
                "posts",
                R"({"_id":"p_old","user_email":"old@example.com"})"
        );
        EXPECT_FALSE(old_index_reference.success);

        const auto new_index_reference = engine.InsertOne(
                "posts",
                R"({"_id":"p_new","user_email":"new@example.com"})"
        );
        ASSERT_TRUE(new_index_reference.success)
                << new_index_reference.error_msg;

        const auto delete_result =
                engine.DeleteById("users", "u1");
        ASSERT_TRUE(delete_result.success)
                << delete_result.error_msg;
        EXPECT_EQ(delete_result.data, "1");

        const auto reference_after_delete = engine.InsertOne(
                "posts",
                R"({"_id":"p_deleted","user_email":"new@example.com"})"
        );
        EXPECT_FALSE(reference_after_delete.success);

        const auto final_count =
                engine.Count("users", Condition{});
        ASSERT_TRUE(final_count.success)
                << final_count.error_msg;
        EXPECT_EQ(final_count.data, "0");
    }

    TEST(DocEngineMutation,
         InjectedFaultsNeverExposePartialDocumentIndexOrCounter) {
        TestTempDir temp("nexora_doc_fault_injection");
        DocEngine engine((temp.path() / "db").string());
        ASSERT_TRUE(engine.CreateCollection("users").success);
        ASSERT_TRUE(engine.CreateCollection("posts").success);

        IndexDefinition index;
        index.index_name = "idx_users_email";
        index.fields = {"email"};
        index.type = IndexType::SingleField;
        ASSERT_TRUE(engine.CreateIndex("users", index).success);

        ForeignKeyDefinition foreign_key;
        foreign_key.fk_name = "fk_posts_user_email";
        foreign_key.local_field = "user_email";
        foreign_key.ref_collection = "users";
        foreign_key.ref_field = "email";
        ASSERT_TRUE(engine.AddForeignKey("posts", foreign_key).success);

        const std::vector<MutationFaultPoint> fault_points = {
                MutationFaultPoint::AfterDocument,
                MutationFaultPoint::AfterIndexes,
                MutationFaultPoint::AfterCounter
        };

        for (std::size_t index_value = 0;
             index_value < fault_points.size();
             ++index_value) {
            const std::string id = "fault_" + std::to_string(index_value);
            const std::string email = id + "@example.com";
            const std::string document =
                    "{\"_id\":\"" + id +
                    "\",\"email\":\"" + email + "\"}";

            engine.SetMutationFaultPointForTesting(
                    fault_points[index_value]);
            const auto failed = engine.InsertOne("users", document);
            EXPECT_FALSE(failed.success);
            EXPECT_NE(failed.error_msg.find("Injected mutation fault"),
                      std::string::npos);
            EXPECT_FALSE(engine.FindById("users", id).success);

            const auto count = engine.Count("users", Condition{});
            ASSERT_TRUE(count.success) << count.error_msg;
            EXPECT_EQ(count.data, "0");

            EXPECT_FALSE(engine.InsertOne(
                    "posts",
                    "{\"_id\":\"post_" + std::to_string(index_value) +
                    "\",\"user_email\":\"" + email + "\"}").success);

            // The one-shot fault is consumed; the same ID must now be usable.
            ASSERT_TRUE(engine.InsertOne("users", document).success);
            ASSERT_TRUE(engine.DeleteById("users", id).success);
        }
    }

    TEST(DocEngineBulkMutation,
         RollsBackOnValidationErrorAndMaintainsIndexesAndCounter) {
        TestTempDir temp("nexora_doc_atomic_bulk_mutation");
        DocEngine engine((temp.path() / "db").string());

        SchemaDefinition schema;
        schema.fields.push_back(SchemaField{
                .name = "name",
                .type = FieldType::String,
                .required = true,
                .unique = false,
                .default_val = std::nullopt
        });

        ASSERT_TRUE(engine.CreateCollection("users", schema).success);
        ASSERT_TRUE(engine.CreateCollection("posts").success);

        IndexDefinition index;
        index.index_name = "idx_users_email";
        index.fields = {"email"};
        index.type = IndexType::SingleField;
        ASSERT_TRUE(engine.CreateIndex("users", index).success);

        ForeignKeyDefinition foreign_key;
        foreign_key.fk_name = "fk_posts_user_email";
        foreign_key.local_field = "user_email";
        foreign_key.ref_collection = "users";
        foreign_key.ref_field = "email";
        ASSERT_TRUE(engine.AddForeignKey("posts", foreign_key).success);

        const auto rejected_batch = engine.InsertMany(
                "users",
                {
                        R"({"_id":"u1","name":"Ali","email":"one@example.com","group":"g"})",
                        R"({"_id":"u2","email":"two@example.com","group":"g"})"
                });
        EXPECT_FALSE(rejected_batch.success);
        EXPECT_FALSE(engine.FindById("users", "u1").success);

        const auto count_after_rejection =
                engine.Count("users", Condition{});
        ASSERT_TRUE(count_after_rejection.success);
        EXPECT_EQ(count_after_rejection.data, "0");

        const auto inserted = engine.InsertMany(
                "users",
                {
                        R"({"_id":"u1","name":"Ali","email":"one@example.com","group":"g"})",
                        R"({"_id":"u2","name":"Sara","email":"two@example.com","group":"g"})"
                });
        ASSERT_TRUE(inserted.success) << inserted.error_msg;

        const auto count_after_insert =
                engine.Count("users", Condition{});
        ASSERT_TRUE(count_after_insert.success);
        EXPECT_EQ(count_after_insert.data, "2");

        const auto duplicate_batch = engine.InsertMany(
                "users",
                {
                        R"({"_id":"u3","name":"Reza","email":"three@example.com","group":"g"})",
                        R"({"_id":"u1","name":"Duplicate","email":"duplicate@example.com","group":"g"})"
                });
        EXPECT_FALSE(duplicate_batch.success);
        EXPECT_FALSE(engine.FindById("users", "u3").success);

        UpdateSpec update;
        update.Set("email", "shared@example.com");

        const auto updated = engine.UpdateMany(
                "users",
                Condition::Leaf("group", Op::EQ, "g"),
                update);
        ASSERT_TRUE(updated.success) << updated.error_msg;
        EXPECT_EQ(updated.data, "2");

        EXPECT_FALSE(engine.InsertOne(
                "posts",
                R"({"_id":"old","user_email":"one@example.com"})"
        ).success);
        ASSERT_TRUE(engine.InsertOne(
                "posts",
                R"({"_id":"current","user_email":"shared@example.com"})"
        ).success);

        const auto deleted = engine.DeleteMany(
                "users",
                Condition::Leaf("group", Op::EQ, "g"));
        ASSERT_TRUE(deleted.success) << deleted.error_msg;
        EXPECT_EQ(deleted.data, "2");

        const auto final_count =
                engine.Count("users", Condition{});
        ASSERT_TRUE(final_count.success);
        EXPECT_EQ(final_count.data, "0");

        EXPECT_FALSE(engine.InsertOne(
                "posts",
                R"({"_id":"after-delete","user_email":"shared@example.com"})"
        ).success);
    }

    TEST(DocEngineBulkMutation,
         OrderedChunksReportCommittedProgressAndStopOnError) {
        TestTempDir temp("nexora_doc_ordered_chunks");
        DocEngine engine((temp.path() / "db").string());

        SchemaDefinition schema;
        schema.fields.push_back(SchemaField{
                .name = "name",
                .type = FieldType::String,
                .required = true,
                .unique = false,
                .default_val = std::nullopt
        });
        ASSERT_TRUE(engine.CreateCollection("items", schema).success);

        BulkWriteOptions options;
        options.mode = BulkWriteMode::OrderedChunks;
        options.max_operations_per_chunk = 2;
        options.max_bytes_per_chunk = 1024 * 1024;

        const auto inserted = engine.InsertManyBulk(
                "items",
                {
                        R"({"_id":"i1","name":"one","group":"g"})",
                        R"({"_id":"i2","name":"two","group":"g"})",
                        R"({"_id":"i3","name":"three","group":"g"})",
                        R"({"_id":"i4","name":"four","group":"g"})",
                        R"({"_id":"i5","name":"five","group":"g"})"
                },
                options);
        ASSERT_TRUE(inserted.success) << inserted.last_error;
        EXPECT_EQ(inserted.processed, 5);
        EXPECT_EQ(inserted.modified, 5);
        EXPECT_EQ(inserted.committed_chunks, 3);

        UpdateSpec update;
        update.Set("state", "updated");
        const auto updated = engine.UpdateManyBulk(
                "items",
                Condition::Leaf("group", Op::EQ, "g"),
                update,
                options);
        ASSERT_TRUE(updated.success) << updated.last_error;
        EXPECT_EQ(updated.modified, 5);
        EXPECT_EQ(updated.committed_chunks, 3);

        const auto partial = engine.InsertManyBulk(
                "items",
                {
                        R"({"_id":"p1","name":"valid"})",
                        R"({"_id":"p2","name":"valid"})",
                        R"({"_id":"p3"})",
                        R"({"_id":"p4","name":"not-attempted"})"
                },
                options);
        EXPECT_FALSE(partial.success);
        EXPECT_EQ(partial.processed, 3);
        EXPECT_EQ(partial.modified, 2);
        EXPECT_EQ(partial.committed_chunks, 1);
        EXPECT_FALSE(partial.last_error.empty());
        EXPECT_TRUE(engine.FindById("items", "p1").success);
        EXPECT_FALSE(engine.FindById("items", "p3").success);
        EXPECT_FALSE(engine.FindById("items", "p4").success);

        const auto deleted = engine.DeleteManyBulk(
                "items",
                Condition::Leaf("group", Op::EQ, "g"),
                options);
        ASSERT_TRUE(deleted.success) << deleted.last_error;
        EXPECT_EQ(deleted.modified, 5);
        EXPECT_EQ(deleted.committed_chunks, 3);

        const auto count = engine.Count("items", Condition{});
        ASSERT_TRUE(count.success) << count.error_msg;
        EXPECT_EQ(count.data, "2");
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
            EXPECT_FALSE(transaction->IsValid());
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
            EXPECT_FALSE(transaction->IsValid());
        }

        EXPECT_FALSE(engine.FindById("users", "rolled_back").success);
    }

    TEST(DocEngineTransaction,
         TxMutationsValidateAndMaintainIndexesAndCounter) {
        TestTempDir temp("nexora_doc_transaction_mutations");
        DocEngine engine((temp.path() / "db").string());

        SchemaDefinition user_schema;
        user_schema.fields.push_back(SchemaField{
                .name = "name",
                .type = FieldType::String,
                .required = true,
                .unique = false,
                .default_val = std::nullopt
        });

        ASSERT_TRUE(engine.CreateCollection("users", user_schema).success);
        ASSERT_TRUE(engine.CreateCollection("posts").success);

        IndexDefinition email_index;
        email_index.index_name = "idx_users_email";
        email_index.fields = {"email"};
        email_index.type = IndexType::SingleField;
        ASSERT_TRUE(engine.CreateIndex("users", email_index).success);

        ForeignKeyDefinition foreign_key;
        foreign_key.fk_name = "fk_posts_email";
        foreign_key.local_field = "user_email";
        foreign_key.ref_collection = "users";
        foreign_key.ref_field = "email";
        ASSERT_TRUE(engine.AddForeignKey("posts", foreign_key).success);

        auto transaction = engine.BeginTransaction();
        ASSERT_NE(transaction, nullptr);

        const auto inserted = engine.InsertOneTx(
                *transaction,
                "users",
                R"({"_id":"u1","name":"Ali","email":"ali@example.com"})");
        ASSERT_TRUE(inserted.success) << inserted.error_msg;

        UpdateSpec invalid_update;
        invalid_update.Unset("name");
        const auto rejected_update = engine.UpdateByIdTx(
                *transaction,
                "users",
                "u1",
                invalid_update);
        EXPECT_FALSE(rejected_update.success);

        const auto still_visible = engine.FindByIdTx(
                *transaction,
                "users",
                "u1");
        ASSERT_TRUE(still_visible.success) << still_visible.error_msg;
        EXPECT_NE(
                still_visible.data.find("\"name\":\"Ali\""),
                std::string::npos);

        // FK validation must see the index entry staged earlier in this same
        // transaction.
        const auto referenced = engine.InsertOneTx(
                *transaction,
                "posts",
                R"({"_id":"p1","user_email":"ali@example.com"})");
        ASSERT_TRUE(referenced.success) << referenced.error_msg;

        const auto committed = engine.CommitTransaction(*transaction);
        ASSERT_TRUE(committed.success) << committed.error_msg;

        const auto user_count = engine.Count("users", Condition{});
        ASSERT_TRUE(user_count.success);
        EXPECT_EQ(user_count.data, "1");

        const auto post_count = engine.Count("posts", Condition{});
        ASSERT_TRUE(post_count.success);
        EXPECT_EQ(post_count.data, "1");

        auto delete_transaction = engine.BeginTransaction();
        ASSERT_NE(delete_transaction, nullptr);
        ASSERT_TRUE(engine.DeleteByIdTx(
                *delete_transaction,
                "users",
                "u1").success);
        ASSERT_TRUE(engine.RollbackTransaction(*delete_transaction).success);

        // Rolling back the outer transaction must restore document, index and
        // counter together.
        EXPECT_TRUE(engine.FindById("users", "u1").success);
        ASSERT_TRUE(engine.InsertOne(
                "posts",
                R"({"_id":"p2","user_email":"ali@example.com"})"
        ).success);

        const auto count_after_rollback =
                engine.Count("users", Condition{});
        ASSERT_TRUE(count_after_rollback.success);
        EXPECT_EQ(count_after_rollback.data, "1");
    }

    TEST(DocEngineTransaction,
         ConflictingWriterReturnsTimeoutWithoutPartialMutation) {
        TestTempDir temp("nexora_doc_transaction_conflict");
        TransactionSettings settings;
        settings.lock_timeout_ms = 50;
        settings.expiration_ms = 5000;
        settings.deadlock_detect = true;
        DocEngine engine((temp.path() / "db").string(), settings);

        ASSERT_TRUE(engine.CreateCollection("items").success);
        ASSERT_TRUE(engine.InsertOne(
                "items",
                R"({"_id":"same","owner":"original"})").success);

        auto first = engine.BeginTransaction();
        auto second = engine.BeginTransaction();
        ASSERT_NE(first, nullptr);
        ASSERT_NE(second, nullptr);

        UpdateSpec first_update;
        first_update.Set("owner", "first");
        ASSERT_TRUE(engine.UpdateByIdTx(
                *first,
                "items",
                "same",
                first_update).success);

        UpdateSpec second_update;
        second_update.Set("owner", "second");
        const auto conflict = engine.UpdateByIdTx(
                *second,
                "items",
                "same",
                second_update);
        EXPECT_FALSE(conflict.success);
        EXPECT_NE(
                conflict.error_msg.find("[transaction-timeout]"),
                std::string::npos) << conflict.error_msg;

        ASSERT_TRUE(engine.RollbackTransaction(*second).success);
        ASSERT_TRUE(engine.RollbackTransaction(*first).success);

        const auto stored = engine.FindById("items", "same");
        ASSERT_TRUE(stored.success) << stored.error_msg;
        EXPECT_NE(stored.data.find("\"owner\":\"original\""),
                  std::string::npos);
    }

    TEST(DocEngineTransaction,
         NormalAndTransactionalValidationHaveEquivalentBehavior) {
        TestTempDir temp("nexora_doc_validation_parity");
        DocEngine engine((temp.path() / "db").string());

        SchemaDefinition schema;
        schema.fields.push_back(SchemaField{
                .name = "name",
                .type = FieldType::String,
                .required = true,
                .unique = false,
                .default_val = std::nullopt
        });
        ASSERT_TRUE(engine.CreateCollection("users", schema).success);
        ASSERT_TRUE(engine.CreateCollection("posts").success);
        ASSERT_TRUE(engine.InsertOne(
                "users",
                R"({"_id":"u1","name":"Ali"})").success);

        ForeignKeyDefinition foreign_key;
        foreign_key.fk_name = "fk_posts_user";
        foreign_key.local_field = "user_id";
        foreign_key.ref_collection = "users";
        foreign_key.ref_field = "_id";
        ASSERT_TRUE(engine.AddForeignKey("posts", foreign_key).success);

        auto transaction = engine.BeginTransaction();
        ASSERT_NE(transaction, nullptr);

        const auto normal_schema = engine.InsertOne(
                "users",
                R"({"_id":"normal_invalid"})");
        const auto tx_schema = engine.InsertOneTx(
                *transaction,
                "users",
                R"({"_id":"tx_invalid"})");
        EXPECT_FALSE(normal_schema.success);
        EXPECT_FALSE(tx_schema.success);
        EXPECT_NE(normal_schema.error_msg.find("Schema validation failed"),
                  std::string::npos);
        EXPECT_NE(tx_schema.error_msg.find("Schema validation failed"),
                  std::string::npos);

        const auto normal_fk = engine.InsertOne(
                "posts",
                R"({"_id":"p_normal","user_id":"missing"})");
        const auto tx_fk = engine.InsertOneTx(
                *transaction,
                "posts",
                R"({"_id":"p_tx","user_id":"missing"})");
        EXPECT_FALSE(normal_fk.success);
        EXPECT_FALSE(tx_fk.success);
        EXPECT_NE(normal_fk.error_msg.find("FK violation"),
                  std::string::npos);
        EXPECT_NE(tx_fk.error_msg.find("FK violation"),
                  std::string::npos);

        UpdateSpec invalid_update;
        invalid_update.Unset("name");
        const auto normal_update = engine.UpdateById(
                "users", "u1", invalid_update);
        const auto tx_update = engine.UpdateByIdTx(
                *transaction, "users", "u1", invalid_update);
        EXPECT_FALSE(normal_update.success);
        EXPECT_FALSE(tx_update.success);
        EXPECT_NE(normal_update.error_msg.find("Schema validation failed"),
                  std::string::npos);
        EXPECT_NE(tx_update.error_msg.find("Schema validation failed"),
                  std::string::npos);

        ASSERT_TRUE(engine.RollbackTransaction(*transaction).success);
        EXPECT_FALSE(engine.FindById("users", "normal_invalid").success);
        EXPECT_FALSE(engine.FindById("users", "tx_invalid").success);
    }

    TEST(DocEngineForeignKey, RejectsNonIndexedReferenceField) {
        TestTempDir temp("nexora_doc_fk_requires_index");
        DocEngine engine((temp.path() / "db").string());

        ASSERT_TRUE(engine.CreateCollection("users").success);
        ASSERT_TRUE(engine.CreateCollection("posts").success);

        ForeignKeyDefinition foreign_key;
        foreign_key.fk_name = "fk_posts_email";
        foreign_key.local_field = "user_email";
        foreign_key.ref_collection = "users";
        foreign_key.ref_field = "email";

        const auto result = engine.AddForeignKey("posts", foreign_key);
        EXPECT_FALSE(result.success);
        EXPECT_NE(result.error_msg.find("requires an index"),
                  std::string::npos);
    }

    TEST(DocEngineCounter,
         ConcurrentInsertAndDeleteDoNotLoseUpdates) {
        TestTempDir temp("nexora_doc_counter_concurrency");
        DocEngine engine((temp.path() / "db").string());
        ASSERT_TRUE(engine.CreateCollection("items").success);

        std::vector<std::string> initial_documents;
        initial_documents.reserve(200);
        for (int index = 0; index < 200; ++index) {
            initial_documents.push_back(
                    R"({"_id":"old_)" +
                    std::to_string(index) +
                    R"(","value":1})");
        }
        ASSERT_TRUE(engine.InsertMany(
                "items",
                initial_documents).success);

        std::atomic<bool> start{false};
        std::atomic<bool> operation_failed{false};
        std::vector<std::thread> workers;

        for (int worker = 0; worker < 2; ++worker) {
            workers.emplace_back([&, worker] {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (int offset = 0; offset < 50; ++offset) {
                    const int index = worker * 50 + offset;
                    const auto result = engine.InsertOne(
                            "items",
                            R"({"_id":"new_)" +
                            std::to_string(index) +
                            R"(","value":2})");
                    if (!result.success) {
                        operation_failed.store(
                                true,
                                std::memory_order_relaxed);
                    }
                }
            });
        }

        for (int worker = 0; worker < 2; ++worker) {
            workers.emplace_back([&, worker] {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (int offset = 0; offset < 50; ++offset) {
                    const int index = worker * 50 + offset;
                    const auto result = engine.DeleteById(
                            "items",
                            "old_" + std::to_string(index));
                    if (!result.success || result.data != "1") {
                        operation_failed.store(
                                true,
                                std::memory_order_relaxed);
                    }
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& worker : workers) {
            worker.join();
        }

        EXPECT_FALSE(operation_failed.load(std::memory_order_relaxed));

        const auto stored_count = engine.Count("items", Condition{});
        ASSERT_TRUE(stored_count.success) << stored_count.error_msg;
        EXPECT_EQ(stored_count.data, "200");
        EXPECT_EQ(engine.GetDocumentRange("items").size(), 200U);

        const auto reconciled =
                engine.ReconcileCollectionCounter("items");
        ASSERT_TRUE(reconciled.success) << reconciled.error_msg;
        EXPECT_EQ(reconciled.data, "200");
    }

    TEST(IndexCodecV2, PreservesTypeOrderAndCompoundPrefix) {
        using nexora::core::indexv2::EncodeValues;
        using nexora::query::FieldValue;

        const auto negative = EncodeValues({FieldValue{"-2", ValueType::Int64, true}});
        const auto positive = EncodeValues({FieldValue{"10", ValueType::Int64, true}});
        ASSERT_TRUE(negative && positive);
        EXPECT_LT(*negative, *positive);

        const auto float_low = EncodeValues({FieldValue{"-1.5", ValueType::Float64, true}});
        const auto float_high = EncodeValues({FieldValue{"2.5", ValueType::Float64, true}});
        ASSERT_TRUE(float_low && float_high);
        EXPECT_LT(*float_low, *float_high);

        const auto compound_prefix = EncodeValues({
                FieldValue{"tenant", ValueType::String, true}});
        const auto compound = EncodeValues({
                FieldValue{"tenant", ValueType::String, true},
                FieldValue{"user@example.com", ValueType::String, true}});
        ASSERT_TRUE(compound_prefix && compound);
        EXPECT_TRUE(compound->starts_with(*compound_prefix));
    }

    TEST(DocEngineUniqueIndex, EnforcesCompoundNullMissingEmptyAndStableId) {
        TestTempDir temp("nexora_doc_unique_semantics");
        const auto path = (temp.path() / "db").string();
        std::string stable_id;
        {
            DocEngine engine(path);
            ASSERT_TRUE(engine.CreateCollection("users").success);
            IndexDefinition index;
            index.index_name = "uniq_tenant_email";
            index.fields = {"tenant", "email"};
            index.type = IndexType::Unique;
            ASSERT_TRUE(engine.CreateIndex("users", index).success);
            const auto indexes = engine.GetIndexes("users");
            ASSERT_EQ(indexes.size(), 1U);
            stable_id = indexes.front().index_id;
            EXPECT_FALSE(stable_id.empty());
            EXPECT_EQ(indexes.front().format_version, 2U);

            EXPECT_TRUE(engine.InsertOne("users", R"({"_id":"m1","tenant":"a"})").success);
            EXPECT_TRUE(engine.InsertOne("users", R"({"_id":"m2","tenant":"a"})").success);
            EXPECT_TRUE(engine.InsertOne("users", R"({"_id":"n1","tenant":"a","email":null})").success);
            EXPECT_TRUE(engine.InsertOne("users", R"({"_id":"n2","tenant":"a","email":null})").success);
            EXPECT_TRUE(engine.InsertOne("users", R"({"_id":"e1","tenant":"a","email":""})").success);
            EXPECT_FALSE(engine.InsertOne("users", R"({"_id":"e2","tenant":"a","email":""})").success);
            EXPECT_TRUE(engine.InsertOne("users", R"({"_id":"c1","tenant":"a","email":"x"})").success);
            EXPECT_TRUE(engine.InsertOne("users", R"({"_id":"c2","tenant":"b","email":"x"})").success);
            EXPECT_FALSE(engine.InsertOne("users", R"({"_id":"c3","tenant":"a","email":"x"})").success);

            UpdateSpec change_email;
            change_email.Set("email", "y");
            ASSERT_TRUE(engine.UpdateById(
                    "users", "c1", change_email).success);
            EXPECT_TRUE(engine.InsertOne(
                    "users",
                    R"({"_id":"c3","tenant":"a","email":"x"})").success);
            EXPECT_FALSE(engine.InsertOne(
                    "users",
                    R"({"_id":"c4","tenant":"a","email":"y"})").success);
            ASSERT_TRUE(engine.DeleteById("users", "c1").success);
            EXPECT_TRUE(engine.InsertOne(
                    "users",
                    R"({"_id":"c4","tenant":"a","email":"y"})").success);
        }
        DocEngine reopened(path);
        ASSERT_EQ(reopened.GetIndexes("users").size(), 1U);
        EXPECT_EQ(reopened.GetIndexes("users").front().index_id, stable_id);
    }

    TEST(DocEngineUniqueIndex, RejectsDuplicateBuildAndConcurrentRace) {
        TestTempDir temp("nexora_doc_unique_race");
        DocEngine engine((temp.path() / "db").string());
        ASSERT_TRUE(engine.CreateCollection("legacy").success);
        ASSERT_TRUE(engine.InsertOne("legacy", R"({"_id":"a","email":"same"})").success);
        ASSERT_TRUE(engine.InsertOne("legacy", R"({"_id":"b","email":"same"})").success);
        IndexDefinition index;
        index.index_name = "uniq_email";
        index.fields = {"email"};
        index.type = IndexType::Unique;
        EXPECT_FALSE(engine.CreateIndex("legacy", index).success);
        EXPECT_TRUE(engine.GetIndexes("legacy").empty());

        ASSERT_TRUE(engine.CreateCollection("race").success);
        ASSERT_TRUE(engine.CreateIndex("race", index).success);
        std::atomic<int> successes{0};
        std::vector<std::thread> writers;
        for (int i = 0; i < 2; ++i) {
            writers.emplace_back([&, i] {
                const auto result = engine.InsertOne(
                        "race", "{\"_id\":\"r" + std::to_string(i) +
                        "\",\"email\":\"same\"}");
                if (result.success) successes.fetch_add(1);
            });
        }
        for (auto& writer : writers) writer.join();
        EXPECT_EQ(successes.load(), 1);
        EXPECT_EQ(engine.Count("race", Condition{}).data, "1");
    }

    TEST(DocEngineUniqueIndex, SchemaUniqueCreatesAndProtectsPhysicalIndex) {
        TestTempDir temp("nexora_doc_schema_unique");
        DocEngine engine((temp.path() / "db").string());
        SchemaDefinition schema;
        schema.fields.push_back(SchemaField{"email", FieldType::String,
                                            false, true, std::nullopt});
        ASSERT_TRUE(engine.CreateCollection("users", schema).success);
        const auto indexes = engine.GetIndexes("users");
        ASSERT_EQ(indexes.size(), 1U);
        EXPECT_EQ(indexes.front().type, IndexType::Unique);
        EXPECT_TRUE(engine.InsertOne("users", R"({"_id":"u1","email":"same"})").success);
        EXPECT_FALSE(engine.InsertOne("users", R"({"_id":"u2","email":"same"})").success);
        EXPECT_FALSE(engine.DropIndex("users", indexes.front().index_name).success);
        EXPECT_TRUE(engine.RebuildIndexes("users").success);
    }

} // namespace
