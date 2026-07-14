//#include <iostream>
//#include <filesystem>
//#include <vector>
//
//#include "core/DocEngine.h"
//#include "query/Condition.h"
//#include "query/UpdateSpec.h"
//
//using namespace nexora::core;
//using namespace nexora::query;
//
//// چاپ نتیجه عملیات
//static void PrintResult(const std::string& label, const DBResult& r) {
//    std::cout << "[" << label << "] "
//              << (r.success ? "OK" : "FAIL");
//    if (!r.data.empty())      std::cout << " | data: " << r.data;
//    if (!r.error_msg.empty()) std::cout << " | error: " << r.error_msg;
//    std::cout << "\n";
//}
//
//// اگر قبلاً تست زدی، پوشه DB را پاک کن تا CreateCollection خطا ندهد
//static void ResetTestDb(const std::string& path) {
//    std::error_code ec;
//    std::filesystem::remove_all(path, ec);
//}
//
//int main() {
//    const std::string db_path = "/tmp/nexora_test_db";
//    ResetTestDb(db_path);
//
//    try {
//        // ── ۱) باز کردن دیتابیس ──
//        DocEngine engine(db_path);
//        std::cout << "Healthy: " << (engine.IsHealthy() ? "yes" : "no") << "\n";
//
//        // ── ۲) ساخت چند کالکشن ──
//
//        // users با schema (اختیاری)
//        SchemaDefinition users_schema;
//        users_schema.fields.push_back({"username", FieldType::String, true, true});
//        users_schema.fields.push_back({"email",    FieldType::String, true, false});
//        users_schema.fields.push_back({"age",      FieldType::Int32,  false, false});
//
//        PrintResult("CreateCollection users",
//                    engine.CreateCollection("users", users_schema));
//
//        // posts بدون schema
//        PrintResult("CreateCollection posts",
//                    engine.CreateCollection("posts"));
//
//        // comments
//        PrintResult("CreateCollection comments",
//                    engine.CreateCollection("comments"));
//
//        // ── ۳) Insert ──
//
//        auto r1 = engine.InsertOne("users",
//                                   R"({"_id":"u1","username":"alice","email":"alice@test.com","age":25})");
//        PrintResult("InsertOne alice", r1);
//
//        auto r2 = engine.InsertOne("users",
//                                   R"({"_id":"u2","username":"bob","email":"bob@test.com","age":30})");
//        PrintResult("InsertOne bob", r2);
//
//        // InsertMany
//        std::vector<std::string> posts = {
//                R"({"_id":"p1","title":"Hello NexoraDB","author_id":"u1","likes":0})",
//                R"({"_id":"p2","title":"NoSQL in C++","author_id":"u1","likes":5})",
//                R"({"_id":"p3","title":"Bob's post","author_id":"u2","likes":2})",
//        };
//        PrintResult("InsertMany posts", engine.InsertMany("posts", posts));
//
//        // اگر _id ندهی، خودش UUID می‌سازد
//        PrintResult("InsertOne auto-id",
//                    engine.InsertOne("comments",
//                                     R"({"post_id":"p1","text":"عالی بود!","user_id":"u2"})"));
//
//        // ── ۴) خواندن ──
//
//        PrintResult("FindById u1",
//                    engine.FindById("users", "u1"));
//
//        // جستجو با شرط
//        auto cond = Condition::Leaf("author_id", Op::EQ, "u1", ValueType::String);
//        PrintResult("FindMany posts by u1",
//                    engine.FindMany("posts", cond));
//
//        // شرط ترکیبی
//        auto age_cond = Condition::And({
//                                               Condition::Leaf("age", Op::GTE, "25", ValueType::Int64),
//                                               Condition::Leaf("username", Op::EQ, "alice", ValueType::String),
//                                       });
//        PrintResult("FindMany users (age>=25 AND alice)",
//                    engine.FindMany("users", age_cond));
//
//        // شمارش
//        PrintResult("Count all users",
//                    engine.Count("users", {}));  // شرط خالی = همه
//
//        std::cout << "GetCollectionSize(posts): "
//                  << engine.GetCollectionSize("posts") << "\n";
//
//        // ── ۵) Update ──
//
//        UpdateSpec spec;
//        spec.Inc("likes", "1", UpdateValueType::Int64);
//        PrintResult("UpdateById p1 likes++",
//                    engine.UpdateById("posts", "p1", spec));
//
//        PrintResult("FindById p1 after update",
//                    engine.FindById("posts", "p1"));
//
//        // ── ۶) Index (اختیاری) ──
//        IndexDefinition idx;
//        idx.index_name = "idx_author";
//        idx.fields     = {"author_id"};
//        idx.type       = IndexType::SingleField;
//        PrintResult("CreateIndex", engine.CreateIndex("posts", idx));
//
//        // ── ۷) Foreign Key (اختیاری) ──
//        ForeignKeyDefinition fk;
//        fk.fk_name        = "fk_author";
//        fk.local_field    = "author_id";
//        fk.ref_collection = "users";
//        fk.ref_field      = "_id";
//        PrintResult("AddForeignKey", engine.AddForeignKey("posts", fk));
//
//        // این باید fail شود چون author_id=u99 وجود ندارد
//        PrintResult("Insert invalid FK",
//                    engine.InsertOne("posts",
//                                     R"({"_id":"p_bad","title":"bad","author_id":"u99"})"));
//
//        // ── ۸) Delete ──
//        PrintResult("DeleteById p3",
//                    engine.DeleteById("posts", "p3"));
//
//        PrintResult("Count posts after delete",
//                    engine.Count("posts", {}));
//
//        // ── ۹) Iterate (مثل GraphEngine) ──
//        std::cout << "\n--- Iterate users ---\n";
//        engine.IterateCollection("users",
//                                 [](const std::string& id, const std::string& doc) -> bool {
//                                     std::cout << "  id=" << id << " doc=" << doc << "\n";
//                                     return true;  // false = توقف زودهنگام
//                                 });
//
//    } catch (const std::exception& e) {
//        std::cerr << "Exception: " << e.what() << "\n";
//        return 1;
//    }
//
//    return 0;
//}

/**
 * @file main.cpp
 * @brief NexoraDB — تست کامل DocEngine + GraphEngine
 *
 * چطور build کنی:
 *   mkdir build && cd build
 *   cmake .. -DNEXORA_BUILD_GRAPH=ON -DCMAKE_BUILD_TYPE=Debug
 *   make -j$(nproc)
 *   ./NexoraDB
 *
 * فقط DocEngine (بدون Graph):
 *   cmake .. -DNEXORA_BUILD_GRAPH=OFF
 */

#include <filesystem>
#include <iostream>
#include <vector>

// ── DocEngine ──
#include "core/DocEngine.h"
#include "query/Condition.h"
#include "query/UpdateSpec.h"

// ── GraphEngine (فقط اگر build شده) ──
#ifdef NEXORA_BUILD_GRAPH
#  include "graph/GraphManager.h"
#  include "graph/StaticGraph.h"
#endif

using namespace nexora::core;
using namespace nexora::query;

// ══════════════════════════════════════════════════════════════
// helpers
// ══════════════════════════════════════════════════════════════

static void printResult(const std::string& label, const DBResult& r) {
    std::cout << "[" << label << "] "
              << (r.success ? "✅ OK" : "❌ FAIL");
    if (!r.data.empty())      std::cout << "  →  " << r.data;
    if (!r.error_msg.empty()) std::cout << "  ERR: " << r.error_msg;
    std::cout << "\n";
}

static void sep(const std::string& title) {
    std::cout << "\n══ " << title << " ══\n";
}

static void resetDb(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

// ══════════════════════════════════════════════════════════════
// §1  تست DocEngine (Document Store)
// ══════════════════════════════════════════════════════════════

static void testDocEngine(DocEngine& engine) {

    // ─── Collection Management ───────────────────────────────
    sep("1. Collection Management");

    SchemaDefinition users_schema;
    users_schema.fields.push_back({"username", FieldType::String, true,  true});
    users_schema.fields.push_back({"email",    FieldType::String, true,  false});
    users_schema.fields.push_back({"age",      FieldType::Int32,  false, false});

    printResult("CreateCollection users",    engine.CreateCollection("users",  users_schema));
    printResult("CreateCollection posts",    engine.CreateCollection("posts"));
    printResult("CreateCollection follows",  engine.CreateCollection("follows"));
    printResult("CreateCollection comments", engine.CreateCollection("comments"));

    // تکراری → باید fail شود
    printResult("CreateCollection users(dup)", engine.CreateCollection("users"));

    std::cout << "CollectionExists users: "
              << (engine.CollectionExists("users") ? "yes" : "no") << "\n";

    auto cols = engine.ListCollections();
    std::cout << "ListCollections: ";
    for (const auto& c : cols) std::cout << c << " ";
    std::cout << "\n";

    // ─── InsertOne ───────────────────────────────────────────
    sep("2. InsertOne / InsertMany");

    // با _id دستی
    printResult("Insert u1 (alice)",
                engine.InsertOne("users",
                                 R"({"_id":"u1","username":"alice","email":"alice@test.com","age":25})"));

    printResult("Insert u2 (bob)",
                engine.InsertOne("users",
                                 R"({"_id":"u2","username":"bob","email":"bob@test.com","age":30})"));

    printResult("Insert u3 (sara)",
                engine.InsertOne("users",
                                 R"({"_id":"u3","username":"sara","email":"sara@test.com","age":22})"));

    // بدون _id → UUID v4 خودکار
    auto auto_r = engine.InsertOne("users",
                                   R"({"username":"auto_user","email":"auto@test.com","age":18})");
    printResult("Insert auto-id user", auto_r);
    std::cout << "  auto-generated id: " << auto_r.data << "\n";

    // InsertMany اتمیک
    std::vector<std::string> posts = {
            R"({"_id":"p1","title":"Hello NexoraDB","author_id":"u1","likes":0,"tags":["db","cpp"]})",
            R"({"_id":"p2","title":"NoSQL in C++",   "author_id":"u1","likes":5,"tags":["cpp"]})",
            R"({"_id":"p3","title":"Bob's post",     "author_id":"u2","likes":2,"tags":["misc"]})",
            R"({"_id":"p4","title":"Sara's post",    "author_id":"u3","likes":8,"tags":["db"]})",
    };
    printResult("InsertMany 4 posts", engine.InsertMany("posts", posts));

    // follows (برای ساخت گراف)
    std::vector<std::string> follows = {
            R"({"_id":"f1","from_id":"u1","to_id":"u2","since":1700000000})",
            R"({"_id":"f2","from_id":"u1","to_id":"u3","since":1700001000})",
            R"({"_id":"f3","from_id":"u2","to_id":"u3","since":1700002000})",
            R"({"_id":"f4","from_id":"u3","to_id":"u1","since":1700003000})",
    };
    printResult("InsertMany 4 follows", engine.InsertMany("follows", follows));

    // ─── FindById ────────────────────────────────────────────
    sep("3. FindById");

    printResult("FindById u1", engine.FindById("users", "u1"));
    printResult("FindById p2", engine.FindById("posts",  "p2"));
    printResult("FindById notexist", engine.FindById("users", "u999"));

    // ─── FindMany ────────────────────────────────────────────
    sep("4. FindMany با شرط");

    // شرط ساده
    auto cond_author = Condition::Leaf("author_id", Op::EQ, "u1", ValueType::String);
    printResult("FindMany posts by u1", engine.FindMany("posts", cond_author));

    // شرط ترکیبی AND
    auto cond_age = Condition::And({
                                           Condition::Leaf("age", Op::GTE, "25", ValueType::Int64),
                                           Condition::Leaf("age", Op::LTE, "35", ValueType::Int64),
                                   });
    printResult("FindMany users age 25-35", engine.FindMany("users", cond_age));

    // شرط OR
    auto cond_or = Condition::Or({
                                         Condition::Leaf("username", Op::EQ, "alice", ValueType::String),
                                         Condition::Leaf("username", Op::EQ, "bob",   ValueType::String),
                                 });
    printResult("FindMany alice OR bob", engine.FindMany("users", cond_or));

    // IN
    auto cond_in = Condition::In("username", {"alice", "sara"});
    printResult("FindMany username IN [alice,sara]", engine.FindMany("users", cond_in));

    // با limit و skip (pagination)
    printResult("FindMany posts limit=2 skip=1",
                engine.FindMany("posts", {}, 2, 1));

    // ─── Count و Exists ──────────────────────────────────────
    sep("5. Count / Exists");

    printResult("Count all users",  engine.Count("users", {}));
    printResult("Count all posts",  engine.Count("posts",  {}));

    auto cond_active = Condition::Leaf("likes", Op::GT, "0", ValueType::Int64);
    printResult("Count posts likes>0", engine.Count("posts", cond_active));

    printResult("Exists alice", engine.Exists("users",
                                              Condition::Leaf("username", Op::EQ, "alice", ValueType::String)));
    printResult("Exists ghost", engine.Exists("users",
                                              Condition::Leaf("username", Op::EQ, "ghost", ValueType::String)));

    std::cout << "GetCollectionSize(posts): "
              << engine.GetCollectionSize("posts") << "\n";

    // ─── UpdateById ──────────────────────────────────────────
    sep("6. UpdateById / UpdateMany");

    {
        UpdateSpec spec;
        spec.Inc("likes", "10", UpdateValueType::Int64);
        spec.Set("updated", "true");
        printResult("UpdateById p1 likes+=10", engine.UpdateById("posts", "p1", spec));
        printResult("FindById p1 after update",  engine.FindById("posts",  "p1"));
    }

    {
        UpdateSpec spec;
        spec.Push("tags", "trending");
        printResult("UpdateById p2 push tag", engine.UpdateById("posts", "p2", spec));
    }

    {
        UpdateSpec spec;
        spec.Inc("likes", "1", UpdateValueType::Int64);
        auto cond = Condition::Leaf("author_id", Op::EQ, "u1", ValueType::String);
        printResult("UpdateMany posts by u1 likes++", engine.UpdateMany("posts", cond, spec));
    }

    // ─── Index ───────────────────────────────────────────────
    sep("7. Index");

    {
        IndexDefinition idx;
        idx.index_name = "idx_author";
        idx.fields     = {"author_id"};
        idx.type       = IndexType::SingleField;
        printResult("CreateIndex idx_author on posts", engine.CreateIndex("posts", idx));
    }
    {
        IndexDefinition idx;
        idx.index_name = "idx_username";
        idx.fields     = {"username"};
        idx.type       = IndexType::Unique;
        printResult("CreateIndex idx_username on users", engine.CreateIndex("users", idx));
    }

    // ─── Foreign Key ─────────────────────────────────────────
    sep("8. Foreign Key");

    {
        ForeignKeyDefinition fk;
        fk.fk_name        = "fk_post_author";
        fk.local_field    = "author_id";
        fk.ref_collection = "users";
        fk.ref_field      = "_id";
        printResult("AddForeignKey posts.author_id→users._id",
                    engine.AddForeignKey("posts", fk));
    }

    // باید fail شود — u99 وجود ندارد
    printResult("Insert post invalid FK (u99) [must FAIL]",
                engine.InsertOne("posts",
                                 R"({"_id":"p_bad","title":"bad","author_id":"u99"})"));

    // باید موفق شود — u1 وجود دارد
    printResult("Insert post valid FK (u1) [must OK]",
                engine.InsertOne("posts",
                                 R"({"_id":"p5","title":"FK test","author_id":"u1","likes":0})"));

    // ─── DeleteById / DeleteMany ──────────────────────────────
    sep("9. DeleteById / DeleteMany");

    printResult("DeleteById p3", engine.DeleteById("posts", "p3"));
    printResult("FindById p3 (should fail)", engine.FindById("posts", "p3"));
    printResult("Count posts after delete", engine.Count("posts", {}));

    auto cond_del = Condition::Leaf("likes", Op::EQ, "0", ValueType::Int64);
    printResult("DeleteMany posts likes=0", engine.DeleteMany("posts", cond_del));
    printResult("Count posts after DeleteMany", engine.Count("posts", {}));

    // ─── Transaction ──────────────────────────────────────────
    sep("10. Transaction (ACID)");

    {
        auto tx = engine.BeginTransaction();
        if (tx) {
            auto r1 = engine.InsertOneTx(*tx, "comments",
                                         R"({"post_id":"p1","text":"تراکنش اول","user_id":"u2"})");
            auto r2 = engine.InsertOneTx(*tx, "comments",
                                         R"({"post_id":"p2","text":"تراکنش دوم","user_id":"u3"})");
            auto commit = engine.CommitTransaction(*tx);
            printResult("Tx Commit (2 inserts)", commit);
        }
    }
    printResult("Count comments after Tx", engine.Count("comments", {}));

    // Rollback
    {
        auto tx = engine.BeginTransaction();
        if (tx) {
            engine.InsertOneTx(*tx, "comments",
                               R"({"post_id":"p1","text":"این rollback میشه","user_id":"u1"})");
            engine.RollbackTransaction(*tx);
            std::cout << "[Tx Rollback] rolled back — count unchanged\n";
        }
    }
    printResult("Count comments after Rollback (should be same)", engine.Count("comments", {}));

    // ─── LookupJoin ──────────────────────────────────────────
    sep("11. LookupJoin");

    auto join = engine.LookupJoin(
            "posts",   "author_id",
            "users",   "_id",
            {}, // شرط خالی = همه posts
            3   // limit
    );
    std::cout << "LookupJoin posts→users: success=" << join.success
              << " records=" << join.records.size() << "\n";
    for (size_t i = 0; i < join.records.size(); ++i)
        std::cout << "  [" << i << "] " << join.records[i].substr(0, 80) << "...\n";

    // ─── IterateCollection (Internal API برای GraphEngine) ────
    sep("12. IterateCollection (Graph Internal API)");

    std::cout << "Iterating users:\n";
    int count = 0;
    engine.IterateCollection("users",
                             [&](const std::string& id, const std::string& doc) -> bool {
                                 std::cout << "  [" << count++ << "] id=" << id
                                           << "  doc=" << doc << "\n";
                                 return true;  // false = early stop
                             });

    std::cout << "Iterating follows:\n";
    engine.IterateCollection("follows",
                             [](const std::string& id, const std::string& doc) -> bool {
                                 std::cout << "  id=" << id << "  doc=" << doc << "\n";
                                 return true;
                             });

    std::cout << "\nGetForeignKeys(posts):\n";
    for (const auto& fk : engine.GetForeignKeys("posts"))
        std::cout << "  " << fk.fk_name << ": "
                  << fk.local_field << " → "
                  << fk.ref_collection << "." << fk.ref_field << "\n";
}

// ══════════════════════════════════════════════════════════════
// §2  تست GraphEngine
// ══════════════════════════════════════════════════════════════

static void testGraphEngine(DocEngine& engine, const std::string& graph_dir) {
#if defined(NEXORA_BUILD_GRAPH)

    sep("13. GraphEngine — تعریف و Build گراف");

    nexora::graph::GraphManager gman(&engine, graph_dir);
    gman.startup();

    // ── تعریف گراف LIVE ──
    nexora::graph::GraphDefinition def;
    def.name                   = "social";
    def.mode                   = nexora::graph::GraphMode::Live;
    def.directed               = true;
    def.heterogeneous          = true;
    def.auto_build_on_startup  = false;  // دستی build می‌کنیم

    // NODE: User از collection users
    nexora::graph::NodeMappingDef user_node;
    user_node.node_type  = "User";
    user_node.collection = "users";
    user_node.key_path   = "_id";
    user_node.properties = {"username", "age"};
    def.node_mappings.push_back(user_node);

    // EDGE روش ۱: collection جداگانه (follows)
    nexora::graph::EdgeMappingDef follows_edge;
    follows_edge.edge_type        = "FOLLOWS";
    follows_edge.collection       = "follows";
    follows_edge.source_path      = "from_id";
    follows_edge.source_node_type = "User";
    follows_edge.target_path      = "to_id";
    follows_edge.target_node_type = "User";
    follows_edge.directed         = true;
    def.edge_mappings.push_back(follows_edge);

    // EDGE روش ۲: FK در سند (posts.author_id → users._id)
    nexora::graph::EdgeMappingDef authored_edge;
    authored_edge.edge_type        = "AUTHORED";
    authored_edge.collection       = "posts";
    authored_edge.source_path      = "author_id";
    authored_edge.source_node_type = "User";
    authored_edge.target_path      = "_id";
    authored_edge.target_node_type = "Post";
    authored_edge.directed         = true;
    def.edge_mappings.push_back(authored_edge);

    // NODE: Post از collection posts
    nexora::graph::NodeMappingDef post_node;
    post_node.node_type  = "Post";
    post_node.collection = "posts";
    post_node.key_path   = "_id";
    post_node.properties = {"title", "likes"};
    def.node_mappings.push_back(post_node);

    bool created = gman.createGraph(def);
    std::cout << "createGraph(social): " << (created ? "OK" : "FAIL") << "\n";

    // ── Build از DocEngine ──
    auto br = gman.buildGraph("social");
    std::cout << "buildGraph: success=" << br.success
              << " nodes=" << br.nodes_built
              << " edges=" << br.edges_built
              << " time=" << br.elapsed_ms << "ms\n";
    if (!br.success) std::cout << "  error: " << br.error_msg << "\n";

    // ── آمار ──
    sep("14. GraphStats");
    auto stats = gman.getStats("social");
    std::cout << "active_nodes="   << stats.active_nodes
              << "  active_edges=" << stats.active_edges
              << "  version="      << stats.version << "\n";

    // ── Traversal روی LiveGraph ──
    sep("15. Traversal — LiveGraph (LockAlgorithm mode)");

    {
        auto [g, lk] = gman.acquireReadLock("social");
        if (g) {
            // فالوهای u1
            auto following = g->neighborsExt("u1", nexora::graph::Direction::Out,
                                              "FOLLOWS", 50);
            std::cout << "u1 FOLLOWS:  ";
            for (const auto& id : following) std::cout << id << " ";
            std::cout << "\n";

            // فالورهای u1
            auto followers = g->neighborsExt("u1", nexora::graph::Direction::In,
                                              "FOLLOWS", 50);
            std::cout << "u1 FOLLOWED BY: ";
            for (const auto& id : followers) std::cout << id << " ";
            std::cout << "\n";

            // پست‌های u1
            auto posts = g->neighborsExt("u1", nexora::graph::Direction::Out,
                                          "AUTHORED", 50);
            std::cout << "u1 AUTHORED: ";
            for (const auto& id : posts) std::cout << id << " ";
            std::cout << "\n";

            // بررسی وجود یال
            std::cout << "Edge u1→u2 FOLLOWS: "
                      << (g->hasEdge("u1","u2","FOLLOWS") ? "yes":"no") << "\n";
            std::cout << "Edge u1→u3 FOLLOWS: "
                      << (g->hasEdge("u1","u3","FOLLOWS") ? "yes":"no") << "\n";
            std::cout << "Edge u2→u1 FOLLOWS: "
                      << (g->hasEdge("u2","u1","FOLLOWS") ? "yes":"no") << "\n";
        } else {
            std::cout << "LiveGraph not ready\n";
        }
    } // lock آزاد می‌شود

    // ── Live Update (insert → گراف خودکار آپدیت می‌شود) ──
    sep("16. Live Update — insert → graph auto-update");

    // یال جدید follow
    engine.InsertOne("follows",
        R"({"_id":"f5","from_id":"u2","to_id":"u1","since":1700009000})");
    // باید به GraphManager اطلاع داده شود
    gman.onDocumentInserted("follows",
        R"({"_id":"f5","from_id":"u2","to_id":"u1","since":1700009000})");

    {
        auto [g, lk] = gman.acquireReadLock("social");
        if (g) {
            auto followers = g->neighborsExt("u1", nexora::graph::Direction::In,
                                              "FOLLOWS", 50);
            std::cout << "u1 FOLLOWED BY after live insert: ";
            for (const auto& id : followers) std::cout << id << " ";
            std::cout << "\n";
        }
    }

    // ── StaticGraph Snapshot (JobAlgorithm mode) ──
    sep("17. StaticGraph Snapshot (JobAlgorithm)");

    auto snapshot = gman.createSnapshot("social");
    if (snapshot) {
        auto snap_stats = snapshot->stats();
        std::cout << "Snapshot: nodes=" << snap_stats.nodeCount
                  << "  edges=" << snap_stats.edgeCount
                  << "  version=" << snap_stats.version << "\n";

        // iterate nodes
        std::cout << "Nodes in snapshot:\n";
        snapshot->forEachNode([&](nexora::graph::DenseId id,
                                   nexora::graph::TypeId  tid) -> bool {
            std::cout << "  DenseId=" << id
                      << "  type=" << snapshot->nodeTypeName(tid)
                      << "  ext=" << snapshot->extId(id) << "\n";
            return true;
        });

        // neighbors در snapshot
        nexora::graph::DenseId u1_id = snapshot->denseId("u1");
        if (u1_id != nexora::graph::kInvalidDenseId) {
            auto nbrs = snapshot->neighbors(u1_id,
                                             nexora::graph::Direction::Out);
            std::cout << "u1 OUT neighbors (snapshot): ";
            for (auto n : nbrs) std::cout << n << "(" << snapshot->extId(n) << ") ";
            std::cout << "\n";
        }

        // export COO
        nexora::graph::GraphExportOptions opts;
        opts.remapNodeIdsToContiguous = true;
        auto coo = snapshot->exportCOO(opts);
        std::cout << "COO export: " << coo.src.size() << " edges\n";
        for (size_t i = 0; i < coo.src.size(); ++i) {
            std::cout << "  " << coo.src[i] << " → " << coo.dst[i]
                      << "  typeId=" << coo.edgeTypeIds[i] << "\n";
        }

        // export CSR
        auto csr = snapshot->exportCSR(opts);
        std::cout << "CSR rowPtr size: " << csr.rowPtr.size() << "\n";
    }

    // ── WAL status ──
    sep("18. WAL Status");
    auto wal = gman.getWalStatus("social");
    std::cout << "WAL: total=" << wal.total_entries
              << "  pending=" << wal.pending_entries
              << "  has_pending=" << (wal.has_pending ? "yes":"no") << "\n";

    // ── listGraphs ──
    std::cout << "Graphs: ";
    for (const auto& g : gman.listGraphs()) std::cout << g << " ";
    std::cout << "\n";

#else
    (void)engine;
    (void)graph_dir;
    std::cout << "\n[GraphEngine] Build with -DNEXORA_BUILD_GRAPH=ON to enable\n";
#endif
}

// ══════════════════════════════════════════════════════════════
// main
// ══════════════════════════════════════════════════════════════

int main() {
    const std::string db_path    = "/tmp/nexora_test_db";
    const std::string graph_dir  = "/tmp/nexora_graph_data";

    // پاک کردن داده‌های قبلی
    resetDb(db_path);
    resetDb(graph_dir);

    std::cout << "══════════════════════════════════════════════\n";
    std::cout << "  NexoraDB Full Test\n";
    std::cout << "══════════════════════════════════════════════\n";

    try {
        // ── باز کردن DocEngine ──
        DocEngine engine(db_path);
        std::cout << "DocEngine healthy: "
                  << (engine.IsHealthy() ? "✅ yes" : "❌ no") << "\n";

        // ── تست DocEngine ──
        testDocEngine(engine);

        // ── تست GraphEngine ──
        testGraphEngine(engine, graph_dir);

    } catch (const std::exception& e) {
        std::cerr << "\n❌ Exception: " << e.what() << "\n";
        return 1;
    }

    std::cout << "\n══════════════════════════════════════════════\n";
    std::cout << "  ✅ All tests completed\n";
    std::cout << "══════════════════════════════════════════════\n";
    return 0;
}