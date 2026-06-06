#include <iostream>
#include <filesystem>
#include <vector>

#include "core/DocEngine.h"
#include "query/Condition.h"
#include "query/UpdateSpec.h"

using namespace nexora::core;
using namespace nexora::query;

// چاپ نتیجه عملیات
static void PrintResult(const std::string& label, const DBResult& r) {
    std::cout << "[" << label << "] "
              << (r.success ? "OK" : "FAIL");
    if (!r.data.empty())      std::cout << " | data: " << r.data;
    if (!r.error_msg.empty()) std::cout << " | error: " << r.error_msg;
    std::cout << "\n";
}

// اگر قبلاً تست زدی، پوشه DB را پاک کن تا CreateCollection خطا ندهد
static void ResetTestDb(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

int main() {
    const std::string db_path = "/tmp/nexora_test_db";
    ResetTestDb(db_path);

    try {
        // ── ۱) باز کردن دیتابیس ──
        DocEngine engine(db_path);
        std::cout << "Healthy: " << (engine.IsHealthy() ? "yes" : "no") << "\n";

        // ── ۲) ساخت چند کالکشن ──

        // users با schema (اختیاری)
        SchemaDefinition users_schema;
        users_schema.fields.push_back({"username", FieldType::String, true, true});
        users_schema.fields.push_back({"email",    FieldType::String, true, false});
        users_schema.fields.push_back({"age",      FieldType::Int32,  false, false});

        PrintResult("CreateCollection users",
                    engine.CreateCollection("users", users_schema));

        // posts بدون schema
        PrintResult("CreateCollection posts",
                    engine.CreateCollection("posts"));

        // comments
        PrintResult("CreateCollection comments",
                    engine.CreateCollection("comments"));

        // ── ۳) Insert ──

        auto r1 = engine.InsertOne("users",
                                   R"({"_id":"u1","username":"alice","email":"alice@test.com","age":25})");
        PrintResult("InsertOne alice", r1);

        auto r2 = engine.InsertOne("users",
                                   R"({"_id":"u2","username":"bob","email":"bob@test.com","age":30})");
        PrintResult("InsertOne bob", r2);

        // InsertMany
        std::vector<std::string> posts = {
                R"({"_id":"p1","title":"Hello NexoraDB","author_id":"u1","likes":0})",
                R"({"_id":"p2","title":"NoSQL in C++","author_id":"u1","likes":5})",
                R"({"_id":"p3","title":"Bob's post","author_id":"u2","likes":2})",
        };
        PrintResult("InsertMany posts", engine.InsertMany("posts", posts));

        // اگر _id ندهی، خودش UUID می‌سازد
        PrintResult("InsertOne auto-id",
                    engine.InsertOne("comments",
                                     R"({"post_id":"p1","text":"عالی بود!","user_id":"u2"})"));

        // ── ۴) خواندن ──

        PrintResult("FindById u1",
                    engine.FindById("users", "u1"));

        // جستجو با شرط
        auto cond = Condition::Leaf("author_id", Op::EQ, "u1", ValueType::String);
        PrintResult("FindMany posts by u1",
                    engine.FindMany("posts", cond));

        // شرط ترکیبی
        auto age_cond = Condition::And({
                                               Condition::Leaf("age", Op::GTE, "25", ValueType::Int64),
                                               Condition::Leaf("username", Op::EQ, "alice", ValueType::String),
                                       });
        PrintResult("FindMany users (age>=25 AND alice)",
                    engine.FindMany("users", age_cond));

        // شمارش
        PrintResult("Count all users",
                    engine.Count("users", {}));  // شرط خالی = همه

        std::cout << "GetCollectionSize(posts): "
                  << engine.GetCollectionSize("posts") << "\n";

        // ── ۵) Update ──

        UpdateSpec spec;
        spec.Inc("likes", "1", UpdateValueType::Int64);
        PrintResult("UpdateById p1 likes++",
                    engine.UpdateById("posts", "p1", spec));

        PrintResult("FindById p1 after update",
                    engine.FindById("posts", "p1"));

        // ── ۶) Index (اختیاری) ──
        IndexDefinition idx;
        idx.index_name = "idx_author";
        idx.fields     = {"author_id"};
        idx.type       = IndexType::SingleField;
        PrintResult("CreateIndex", engine.CreateIndex("posts", idx));

        // ── ۷) Foreign Key (اختیاری) ──
        ForeignKeyDefinition fk;
        fk.fk_name        = "fk_author";
        fk.local_field    = "author_id";
        fk.ref_collection = "users";
        fk.ref_field      = "_id";
        PrintResult("AddForeignKey", engine.AddForeignKey("posts", fk));

        // این باید fail شود چون author_id=u99 وجود ندارد
        PrintResult("Insert invalid FK",
                    engine.InsertOne("posts",
                                     R"({"_id":"p_bad","title":"bad","author_id":"u99"})"));

        // ── ۸) Delete ──
        PrintResult("DeleteById p3",
                    engine.DeleteById("posts", "p3"));

        PrintResult("Count posts after delete",
                    engine.Count("posts", {}));

        // ── ۹) Iterate (مثل GraphEngine) ──
        std::cout << "\n--- Iterate users ---\n";
        engine.IterateCollection("users",
                                 [](const std::string& id, const std::string& doc) -> bool {
                                     std::cout << "  id=" << id << " doc=" << doc << "\n";
                                     return true;  // false = توقف زودهنگام
                                 });

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return 1;
    }

    return 0;
}