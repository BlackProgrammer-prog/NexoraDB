#include "SecureDatabase.h"
#include <nlohmann/json.hpp>
#include <sodium.h>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <shared_mutex>
#include <thread>

namespace nexora::security {
namespace {
using Json=nlohmann::json;
std::string fingerprint(const std::string& value) {
    unsigned char hash[32];char hex[65];crypto_generichash(hash,sizeof hash,
        reinterpret_cast<const unsigned char*>(value.data()),value.size(),nullptr,0);
    sodium_bin2hex(hex,sizeof hex,hash,sizeof hash);return hex;
}
bool oneOf(const std::string& value,std::initializer_list<const char*> names) {
    return std::any_of(names.begin(),names.end(),[&](const auto* name){return value==name;});
}
}
struct SecureDatabase::Impl {
    struct Cursor {std::string owner,collection;std::chrono::steady_clock::time_point expires;
        std::vector<std::array<std::string,3>> joins;};
    std::mutex mutex;
    std::shared_mutex schema;
    std::shared_ptr<std::atomic<unsigned>> transactions=std::make_shared<std::atomic<unsigned>>(0);
    std::unordered_map<std::string,Cursor> pages;
};
struct SecureTransaction::Impl {
    std::shared_ptr<SecureDatabase> owner;
    std::unique_ptr<core::TxHandle> handle;
    std::shared_ptr<std::atomic<unsigned>> active;
    std::string session;
    uint64_t epoch;
    std::unordered_set<std::string> touched;
    std::thread::id thread=std::this_thread::get_id();
    std::chrono::steady_clock::time_point expires=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    ~Impl() {if(handle) {handle.reset();--*active;}}
    void close() {handle.reset();--*active;}
};
SecureTransaction::SecureTransaction(std::unique_ptr<Impl> value):impl_(std::move(value)) {}
SecureTransaction::~SecureTransaction()=default;
bool SecureTransaction::isValid() const noexcept {
    return impl_ && impl_->thread==std::this_thread::get_id() && impl_->handle && impl_->handle->IsValid() &&
        std::chrono::steady_clock::now()<impl_->expires;
}
SecureDatabase::SecureDatabase(const std::string& path,const core::RocksSettings& settings,SessionConfig sessions)
    :engine_(new core::DocEngine(path,{},settings,true)),
     authority_(std::make_unique<SessionAuthority>(*engine_->txn_db_,sessions)),impl_(std::make_unique<Impl>()) {}
SecureDatabase::~SecureDatabase() {
    if(authority_->commitUncertain()) {
        std::lock_guard lock(engine_->operations_.mutex);
        engine_->operations_.io_error=true;
    }
}
void SecureDatabase::refreshProjection(const std::string& collection) {
    std::function<void(const std::string&)> callback;
    {std::lock_guard lock(projection_mutex_);callback=refresh_projection_;}
    if(callback) callback(collection);
}
bool SecureDatabase::ready() const noexcept {return !authority_->commitUncertain() && engine_->IsHealthy();}
core::DBResult SecureDatabase::prepareShutdown() {
    if(authority_->commitUncertain()) return core::DBResult::Err("IAM commit outcome uncertain; offline inspection required","io_error");
    return engine_->PrepareShutdown();
}
std::string SecureDatabase::login(const std::string& name,const std::string& password,const std::string& source) {return authority_->login(name,password,source);}
std::string SecureDatabase::claims(const std::string& token){return authority_->claims(token);}
void SecureDatabase::logout(const std::string& token){authority_->logout(token);}
void SecureDatabase::changePassword(const std::string& token,const std::string& old,const std::string& replacement){authority_->changePassword(token,old,replacement);}
void SecureDatabase::completeReset(const std::string& token,const std::string& replacement){authority_->completeReset(token,replacement);}
std::string SecureDatabase::administer(const std::string& token,const std::string& operation,const std::string& request){return authority_->administer(token,operation,request);}

DataReply SecureDatabase::execute(const std::string& token,const DataRequest& r) {
    const auto& op=r.operation;auto& e=*engine_;
    std::string action,scope;
    const bool transactionOperation=op.ends_with("_tx") || op=="commit_transaction" || op=="rollback_transaction";
    if(oneOf(op,{"find_by_id","find_many","find_page","count","exists","lookup_join","find_by_id_tx"})) {action="read";scope="documents:read";}
    else if(oneOf(op,{"insert_one","insert_many","insert_many_bson","update_by_id","update_many","delete_by_id","delete_many",
        "insert_one_tx","update_by_id_tx","delete_by_id_tx"})) {action="write";scope="documents:write";}
    else if(oneOf(op,{"collection_exists","get_schema","get_indexes","get_foreign_keys","get_collection_size"})) {action="read";scope="collections:read";}
    else if(oneOf(op,{"create_collection","drop_collection","set_schema","create_index","drop_index","rebuild_indexes","resume_index_build","cleanup_index_build","add_foreign_key","drop_foreign_key"})) {action="manage";scope="collections:write";}
    else if(op=="list_collections") {action="session";scope="collections:read";}
    else if(oneOf(op,{"close_page","begin_transaction","commit_transaction","rollback_transaction","sync_ack"})) action="session";
    else if(oneOf(op,{"is_healthy","rocks_metrics","health_report","memory_report","durability_report","operation_metrics",
        "get_ram_usage_bytes","get_disk_usage_bytes","query_tuning","configure_query_tuning","drain_slow_queries",
        "configure_slow_queries","flush_storage","integrity_check","create_checkpoint","format_versions","prepare_shutdown","explain_plan"})) action="admin";
    else throw AuthorizationError("Unsupported operation at native IAM boundary");

    {auto admission=authority_->authorize(token,action,
        (action=="session" || action=="admin")?"":r.collection,scope);}
    std::unique_lock schemaWrite(impl_->schema,std::defer_lock);
    std::shared_lock schemaRead(impl_->schema,std::defer_lock);
    if(action=="manage") {
        schemaWrite.lock();
        if(impl_->transactions->load()!=0) throw RateLimitError("Schema change requires active transactions to finish");
    } else if(!transactionOperation) schemaRead.lock();

    std::vector<std::pair<std::string,std::string>> resources{{action,(action=="session" || action=="admin")?"":r.collection}};
    if(op=="lookup_join") resources.emplace_back("read",r.target_collection);
    for(const auto& join:r.joins) resources.emplace_back("read",join[0]);
    if(op=="add_foreign_key") resources.emplace_back("read",r.foreign_key.ref_collection);
    // Constraint validation must not become an existence oracle for collections
    // the caller cannot read. Inspect metadata internally, then check all sides.
    if(action=="write") {
        for(const auto& fk:e.GetForeignKeys(r.collection)) resources.emplace_back("read",fk.ref_collection);
        for(const auto& name:e.ListCollections()) for(const auto& fk:e.GetForeignKeys(name))
            if(fk.ref_collection==r.collection) resources.emplace_back("read",name);
    }
    if(op=="list_collections") {
        // Release the admission lease before acquiring the filtered-list lease.
        {auto lease=authority_->authorize(token,"session","",scope);}
        return authority_->visibleCollections(token,e.ListCollections());
    }
    auto lease=authority_->authorizeMany(token,resources,scope);
    auto deadline=core::current_execution_control?core::current_execution_control:std::make_shared<core::ExecutionControl>(30000);
    core::ExecutionGuard execution(deadline);
    if(op=="begin_transaction") {
        auto tx=std::make_unique<SecureTransaction::Impl>();tx->owner=shared_from_this();
        tx->session=fingerprint(token);tx->epoch=lease.policy_epoch;tx->active=impl_->transactions;
        auto active=tx->active->load();
        do {if(active>=64) throw RateLimitError("Native transaction capacity exceeded");}
        while(!tx->active->compare_exchange_weak(active,active+1));
        try {tx->handle=e.BeginTransaction();if(!tx->handle) throw std::runtime_error("Unable to begin native transaction");}
        catch(...) {--*tx->active;throw;}
        return std::shared_ptr<SecureTransaction>(new SecureTransaction(std::move(tx)));
    }
    if(transactionOperation) {
        if(!r.transaction || !r.transaction->impl_) throw AuthorizationError("Validated transaction required");
        auto& tx=*r.transaction->impl_;
        if(tx.owner.get()!=this || tx.session!=fingerprint(token)) throw AuthorizationError("Transaction belongs to another session");
        if(tx.thread!=std::this_thread::get_id()) throw AuthorizationError("Native transactions are confined to their creating thread");
        if(!tx.handle) throw AuthorizationError("Transaction is closed");
        if(op=="rollback_transaction") {auto result=e.RollbackTransaction(*tx.handle);tx.close();return result;}
        if(tx.epoch!=lease.policy_epoch || std::chrono::steady_clock::now()>=tx.expires) {
            e.RollbackTransaction(*tx.handle);tx.close();throw AuthorizationError("Transaction expired or security policy changed; rolled back");
        }
        if(op=="commit_transaction") {auto result=e.CommitTransaction(*tx.handle);tx.close();
            if(result.success) for(const auto& name:tx.touched) refreshProjection(name);
            return result;
        }
        if(op=="find_by_id_tx") return e.FindByIdTx(*tx.handle,r.collection,r.id);
        tx.touched.insert(r.collection);
        if(op=="insert_one_tx") return e.InsertOneTx(*tx.handle,r.collection,r.payload);
        if(op=="update_by_id_tx") return e.UpdateByIdTx(*tx.handle,r.collection,r.id,r.update);
        if(op=="delete_by_id_tx") return e.DeleteByIdTx(*tx.handle,r.collection,r.id);
    }
    if(op=="find_by_id") return e.FindById(r.collection,r.id);
    if(op=="find_many") return e.FindMany(r.collection,r.condition,r.limit,r.skip);
    if(op=="count") return e.Count(r.collection,r.condition);
    if(op=="exists") return e.Exists(r.collection,r.condition);
    if(op=="get_collection_size") return e.GetCollectionSize(r.collection);
    if(action=="write") {
        core::DBResult result;
        if(op=="insert_one") result=e.InsertOne(r.collection,r.payload);
        else if(op=="insert_many") result=e.InsertMany(r.collection,r.documents);
        else if(op=="insert_many_bson") result=e.InsertManyBson(r.collection,r.documents);
        else if(op=="update_by_id") result=e.UpdateById(r.collection,r.id,r.update);
        else if(op=="update_many") result=e.UpdateMany(r.collection,r.condition,r.update);
        else if(op=="delete_by_id") result=e.DeleteById(r.collection,r.id);
        else if(op=="delete_many") result=e.DeleteMany(r.collection,r.condition);
        if(result.success) refreshProjection(r.collection);
        return result;
    }
    if(op=="create_collection") return e.CreateCollection(r.collection,r.schema);
    if(op=="drop_collection") return e.DropCollection(r.collection);
    if(op=="collection_exists") return e.CollectionExists(r.collection);
    if(op=="get_schema") return e.GetSchema(r.collection);
    if(op=="set_schema") {if(!r.schema) throw std::invalid_argument("Schema required");return e.SetSchemaValidation(r.collection,*r.schema);}
    if(op=="get_indexes") return e.GetIndexes(r.collection);
    if(op=="create_index") return e.CreateIndex(r.collection,r.index);
    if(op=="drop_index") return e.DropIndex(r.collection,r.id);
    if(op=="rebuild_indexes") return e.RebuildIndexes(r.collection);
    if(op=="resume_index_build") return e.ResumeIndexBuild(r.collection,r.id);
    if(op=="cleanup_index_build") return e.CleanupIndexBuild(r.collection,r.id);
    if(op=="get_foreign_keys") return e.GetForeignKeys(r.collection);
    if(op=="add_foreign_key") return e.AddForeignKey(r.collection,r.foreign_key);
    if(op=="drop_foreign_key") return e.DropForeignKey(r.collection,r.id);
    if(op=="lookup_join") {
        auto result=e.LookupJoin(r.collection,r.source_field,r.target_collection,r.target_field,r.condition,r.limit);
        return core::DBResult::Ok(Json({{"success",result.success},{"records",result.records},{"error_msg",result.error_msg}}).dump());
    }
    if(op=="find_page" || op=="close_page") {
        const auto owner=fingerprint(token);const auto now=std::chrono::steady_clock::now();
        {std::lock_guard guard(impl_->mutex);
            for(auto it=impl_->pages.begin();it!=impl_->pages.end();) {
                if(it->second.expires<now) it=impl_->pages.erase(it);else ++it;
            }
            if(!r.page_token.empty()) {
                const auto it=impl_->pages.find(r.page_token);
                if(it==impl_->pages.end() || it->second.owner!=owner || (op=="find_page" &&
                    (it->second.collection!=r.collection || it->second.joins!=r.joins)))
                    throw AuthorizationError("Cursor does not belong to this session");
            }
            if(impl_->pages.size()>=4096 && r.page_token.empty()) throw RateLimitError("Cursor capacity exceeded");
        }
        if(op=="close_page") {e.ClosePage(r.page_token);std::lock_guard guard(impl_->mutex);impl_->pages.erase(r.page_token);return core::DBResult::Ok();}
        auto result=e.FindPage(r.collection,r.condition,r.limit,r.page_token,r.max_bytes,r.joins);
        if(result.success) {
            std::lock_guard guard(impl_->mutex);impl_->pages.erase(r.page_token);
            if(!result.continuation_token.empty()) impl_->pages[result.continuation_token]={owner,r.collection,now+std::chrono::minutes(5),r.joins};
        }
        return result;
    }
    if(op=="is_healthy") return e.IsHealthy();
    if(op=="rocks_metrics") return e.GetRocksMetrics();
    if(op=="health_report") return core::DBResult::Ok(e.HealthReport());
    if(op=="memory_report") return core::DBResult::Ok(e.MemoryReport());
    if(op=="durability_report") return core::DBResult::Ok(e.DurabilityReport());
    if(op=="operation_metrics") return core::DBResult::Ok(e.OperationMetrics());
    if(op=="get_ram_usage_bytes") return static_cast<int64_t>(e.GetRamUsageBytes());
    if(op=="get_disk_usage_bytes") return static_cast<int64_t>(e.GetDiskUsageBytes());
    if(op=="query_tuning") return core::DBResult::Ok(e.QueryTuning());
    if(op=="configure_query_tuning") {e.ConfigureQueryTuning(r.flag,r.second_flag);return core::DBResult::Ok();}
    if(op=="drain_slow_queries") return core::DBResult::Ok(e.DrainSlowQueries());
    if(op=="configure_slow_queries") {e.ConfigureSlowQueries(r.max_bytes,r.limit);return core::DBResult::Ok();}
    if(op=="flush_storage") return e.FlushStorage();
    if(op=="sync_ack") return e.FlushStorage();
    if(op=="integrity_check") return e.IntegrityCheck(r.flag);
    if(op=="create_checkpoint") return e.CreateCheckpoint(r.payload);
    if(op=="format_versions") return e.FormatVersions();
    if(op=="prepare_shutdown") return e.PrepareShutdown();
    if(op=="explain_plan") return e.ExplainPlan(r.collection,r.condition,r.flag);
    throw AuthorizationError("Unsupported native operation");
}
} // namespace nexora::security
