#pragma once
#include "core/DocEngine.h"
#include "core/DocumentCodec.h"
#include <rocksdb/env.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <algorithm>
#include <stdexcept>

// Test-only helpers shared with the separately exec'd crash worker.
namespace recovery_test {
using namespace nexora::core;
using Json = nlohmann::json;
inline void require(bool ok, const std::string& error) {
    if (!ok) throw std::runtime_error(error);
}
inline void checked(const DBResult& result) { require(result.success, result.error_msg); }

class FaultEnv : public rocksdb::EnvWrapper {
public:
    // 1=ENOSPC append, 2=EACCES append, 3=sync error, 4=partial append; arm after setup.
    std::atomic<int> failure{0};
    std::atomic<int> hits{0};
    // 1=before append, 2=after append, 3=before sync, 4=after sync.
    int crash_stage = 0;
    void (*crash)() noexcept = nullptr;
    FaultEnv() : EnvWrapper(rocksdb::Env::Default()) {}
    void boundary(int stage) { if (crash && crash_stage == stage) crash(); }
    rocksdb::Status error() {
        ++hits;
        return failure == 1 ? rocksdb::Status::NoSpace("COR007 ENOSPC")
                            : rocksdb::Status::IOError("COR007 EACCES: Permission denied");
    }
    struct File : rocksdb::WritableFileWrapper {
        std::unique_ptr<rocksdb::WritableFile> owned;
        FaultEnv& env;
        File(std::unique_ptr<rocksdb::WritableFile> file, FaultEnv& e)
            : WritableFileWrapper(file.get()), owned(std::move(file)), env(e) {}
        template<class Append> rocksdb::Status append(Append action) {
            env.boundary(1);
            if (env.failure == 1 || env.failure == 2) return env.error();
            auto status = action();
            if (status.ok()) env.boundary(2);
            return status;
        }
        rocksdb::Status Append(const rocksdb::Slice& data) override {
            if (env.failure == 4) return partial(data);
            return append([&] { return owned->Append(data); });
        }
        rocksdb::Status Append(const rocksdb::Slice& data, const rocksdb::DataVerificationInfo& info) override {
            if (env.failure == 4) return partial(data);
            return append([&] { return owned->Append(data, info); });
        }
        rocksdb::Status partial(const rocksdb::Slice& data) {
            ++env.hits;
            auto status = owned->Append(rocksdb::Slice(data.data(), data.size() / 2));
            return status.ok() ? rocksdb::Status::IOError("COR007 partial WAL append") : status;
        }
        rocksdb::Status sync(bool fsync) {
            env.boundary(3);
            if (env.failure == 3) { ++env.hits; return rocksdb::Status::IOError("COR007 WAL sync failure"); }
            auto status = fsync ? owned->Fsync() : owned->Sync();
            if (status.ok()) env.boundary(4);
            return status;
        }
        rocksdb::Status Sync() override { return sync(false); }
        rocksdb::Status Fsync() override { return sync(true); }
    };
    rocksdb::Status NewWritableFile(const std::string& path, std::unique_ptr<rocksdb::WritableFile>* file,
                                    const rocksdb::EnvOptions& options) override {
        auto status = target()->NewWritableFile(path, file, options);
        if (status.ok() && path.ends_with(".log")) *file = std::make_unique<File>(std::move(*file), *this);
        return status;
    }
};
inline RocksSettings settings(FaultEnv* env = nullptr) {
    RocksSettings rocks;
    rocks.env_for_testing = env;
    rocks.durability = DocumentDurability::Durable;
    rocks.memory_budget_bytes = 16 * 1024 * 1024;
    return rocks;
}
inline Json row(const std::string& id, const std::string& key) {
    return Json{{"_id", id}, {"key", key}, {"nested", {{"values", {1, nullptr, true}}}}};
}
inline void seed(DocEngine& db) {
    db.PauseDocumentMigration();
    checked(db.CreateCollection("items"));
    IndexDefinition index;
    index.index_name = "key_idx"; index.fields = {"key"};
    checked(db.CreateIndex("items", index));
    checked(db.InsertMany("items", {row("a", "old-a").dump(), row("b", "old-b").dump()}));
}
inline nexora::query::Condition eq(const std::string& value) {
    return nexora::query::Condition::Leaf("key", nexora::query::Op::EQ, value);
}
inline std::string bson(const Json& document) {
    auto encoded = DocumentCodec::EncodeJson(document.dump());
    require(encoded.success, encoded.error);
    return encoded.value.substr(DocumentCodec::kEnvelopeSize);
}
// Every operation changes a known complete state; the mixed Tx exercises all three counters/index paths.
inline DBResult mutate(DocEngine& db, const std::string& mode) {
    nexora::query::UpdateSpec update; update.Set("key", "new-a");
    if (mode == "insert") return db.InsertOne("items", row("c", "new-c").dump());
    if (mode == "json") return db.InsertMany("items", {row("c", "new-c").dump(), row("d", "new-d").dump()});
    if (mode == "bson") return db.InsertManyBson("items", {bson(row("c", "new-c")), bson(row("d", "new-d"))});
    if (mode == "ingest") return db.InsertManyBsonIngest("items", {bson(row("c", "new-c")), bson(row("d", "new-d"))});
    if (mode == "update") return db.UpdateById("items", "a", update);
    if (mode == "delete") return db.DeleteById("items", "a");
    if (mode == "update_many") return db.UpdateMany("items", eq("old-a"), update);
    if (mode == "delete_many") return db.DeleteMany("items", {});
    if (mode == "tx") {
        auto tx = db.BeginTransaction(); require(bool(tx), "begin transaction failed");
        auto result = db.InsertOneTx(*tx, "items", row("c", "new-c").dump());
        if (!result.success) { checked(db.RollbackTransaction(*tx)); return result; }
        result = db.UpdateByIdTx(*tx, "items", "a", update);
        if (!result.success) { checked(db.RollbackTransaction(*tx)); return result; }
        result = db.DeleteByIdTx(*tx, "items", "b");
        if (!result.success) { checked(db.RollbackTransaction(*tx)); return result; }
        return db.CommitTransaction(*tx);
    }
    throw std::runtime_error("unknown operation: " + mode);
}
inline Json expected(const std::string& mode, bool committed) {
    Json rows = Json::array({row("a", "old-a"), row("b", "old-b")});
    if (!committed) return rows;
    if (mode == "delete_many") return Json::array();
    if (mode == "update" || mode == "update_many" || mode == "tx") rows[0]["key"] = "new-a";
    if (mode == "delete") rows.erase(rows.begin());
    if (mode == "tx") rows.erase(rows.begin() + 1);
    if (mode == "insert" || mode == "json" || mode == "bson" || mode == "ingest" || mode == "tx") rows.push_back(row("c", "new-c"));
    if (mode == "json" || mode == "bson" || mode == "ingest") rows.push_back(row("d", "new-d"));
    return rows;
}
inline Json snapshot(DocEngine& db) {
    auto result = db.FindManyFullScanForTesting("items", {}); checked(result);
    return Json::parse(result.data);
}
inline void verify(DocEngine& db, const Json& rows) {
    require(snapshot(db) == rows, "full document oracle mismatch");
    auto count = db.Count("items", {}); checked(count);
    require(count.data == std::to_string(rows.size()), "counter mismatch");
    require(db.GetCollectionSize("items") == static_cast<int64_t>(rows.size()), "size mismatch");
    auto page = db.FindPage("items", {}, 100);
    require(page.success && Json::parse(page.data) == rows, "page mismatch");
    for (const auto& key : {"old-a", "old-b", "new-a", "new-c", "new-d"}) {
        Json matches = Json::array();
        for (const auto& doc : rows) if (doc["key"] == key) matches.push_back(doc);
        auto result = db.FindMany("items", eq(key)); checked(result);
        require(Json::parse(result.data) == matches, "index mismatch: " + std::string(key));
        auto plan = db.ExplainPlan("items", eq(key)); checked(plan);
        require(Json::parse(plan.data)["scan_type"] == "index_equality", "index was bypassed");
    }
    for (const auto& id : {"a", "b", "c", "d"}) {
        const auto it = std::find_if(rows.begin(), rows.end(), [&](const Json& doc) { return doc["_id"] == id; });
        const auto result = db.FindById("items", id);
        require(result.success == (it != rows.end()), "point presence mismatch");
        if (result.success) require(Json::parse(result.data) == *it, "point payload mismatch");
    }
    checked(db.IntegrityCheck());
}
} // namespace recovery_test
