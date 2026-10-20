#include "DocEngine.h"
#include <algorithm>
#include <limits>
namespace nexora::core {
namespace {
const char* names[]={"query","point_read","insert","batch_insert","update","delete"};
}
void DocEngine::RecordOperation(OperationsState::Kind kind,uint64_t us,const DBResult& result,const nlohmann::json& plan) const noexcept {
    try {
        std::lock_guard lock(operations_.mutex); auto& item=operations_.counters[kind];
        ++item.calls; item.not_found+=result.error_code=="not_found";
        item.errors+=!result.success && result.error_code!="not_found";
        item.sum_us+=us; item.max_us=std::max(item.max_us,us);
        size_t bucket=0; while(bucket<19 && us>(uint64_t(1)<<bucket)) ++bucket;
        ++item.buckets[bucket];
        operations_.corruption|=result.error_code=="corruption" || result.error_msg.find("Corruption:")!=std::string::npos;
        operations_.io_error|=result.error_code=="io_error" || result.error_msg.find("IO error:")!=std::string::npos;
        const auto threshold=operations_.slow_us.load();
        if(kind==OperationsState::Query && threshold && us>=threshold && operations_.capacity) {
            auto event=nlohmann::json{{"event","slow_query"},{"duration_us",us},{"success",result.success},
                {"sequence",item.calls},{"uptime_seconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-operations_.started).count()},
                {"explain",plan}};
            if(operations_.slow_events.size()==operations_.capacity) { operations_.slow_events.pop_front(); ++operations_.dropped; }
            operations_.slow_events.push_back(std::move(event));
        }
    } catch(...) {} // Telemetry cannot turn a successful transaction into a failure.
}
DBResult DocEngine::InsertOne(const std::string& collection,const std::string& doc) {
    return Observe(OperationsState::Insert,[&]{return InsertOneImpl(collection,doc);});
}
DBResult DocEngine::InsertMany(const std::string& collection,const std::vector<std::string>& docs) {
    return Observe(OperationsState::BatchInsert,[&]{return InsertManyImpl(collection,docs);});
}
DBResult DocEngine::FindById(const std::string& collection,const std::string& id) {
    return Observe(OperationsState::PointRead,[&]{return FindByIdImpl(collection,id);});
}
DBResult DocEngine::UpdateById(const std::string& collection,const std::string& id,const query::UpdateSpec& update) {
    return Observe(OperationsState::Update,[&]{return UpdateByIdImpl(collection,id,update);});
}
DBResult DocEngine::DeleteById(const std::string& collection,const std::string& id) {
    return Observe(OperationsState::Delete,[&]{return DeleteByIdImpl(collection,id);});
}
DocEngine::QueryExecution DocEngine::ExecuteQuery(const std::string& collection,const query::Condition& condition,
        uint32_t limit,uint32_t skip,bool force,const rocksdb::Snapshot* epoch,bool locked) {
    const auto started=std::chrono::steady_clock::now();
    const auto elapsed=[&]{return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count();};
    try {
        auto execution=ExecuteQueryImpl(collection,condition,limit,skip,force,epoch,locked);
        const auto us=elapsed(); nlohmann::json plan;
        const auto threshold=operations_.slow_us.load();
        try { if(threshold && uint64_t(us)>=threshold) plan={{"scan_type",execution.scan_type},{"index",execution.index_name.substr(0,128)},
            {"scanned_index_entries",execution.scanned_index_entries},{"scanned_documents",execution.scanned_documents},
            {"matched_documents",execution.matched_documents}}; } catch(...) {}
        RecordOperation(OperationsState::Query,us,execution.result,plan); return execution;
    } catch(...) { RecordOperation(OperationsState::Query,elapsed(),DBResult::Err("exception")); throw; }
}
void DocEngine::ConfigureSlowQueries(uint64_t threshold,size_t capacity) {
    if(capacity>4096) throw std::invalid_argument("slow query capacity exceeds 4096");
    std::lock_guard lock(operations_.mutex); operations_.slow_us=threshold; operations_.capacity=capacity;
    while(operations_.slow_events.size()>capacity) {operations_.slow_events.pop_front(); ++operations_.dropped;}
}
std::string DocEngine::DrainSlowQueries() {
    std::lock_guard lock(operations_.mutex); nlohmann::json output=nlohmann::json::array();
    for(auto& event:operations_.slow_events) output.push_back(std::move(event));
    operations_.slow_events.clear(); return output.dump();
}
std::string DocEngine::OperationMetrics() const {
    std::lock_guard lock(operations_.mutex);
    const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-operations_.started).count();
    nlohmann::json output={{"uptime_seconds",seconds},{"slow_events_dropped",operations_.dropped},{"operations",nlohmann::json::object()}};
    for(size_t kind=0;kind<OperationsState::Count;++kind) {
        const auto& item=operations_.counters[kind]; nlohmann::json buckets=nlohmann::json::array(); uint64_t cumulative=0;
        for(size_t i=0;i<20;++i) {cumulative+=item.buckets[i]; buckets.push_back({{"le_seconds",i==19?nlohmann::json("+Inf"):nlohmann::json(double(uint64_t(1)<<i)/1e6)},{"count",cumulative}});}
        output["operations"][names[kind]]={{"calls_total",item.calls},{"errors_total",item.errors},{"not_found_total",item.not_found},
            {"duration_seconds_sum",double(item.sum_us)/1e6},{"max_seconds",double(item.max_us)/1e6},{"buckets",buckets},
            {"throughput_per_second",seconds>0?item.calls/seconds:0},{"error_rate",item.calls?double(item.errors)/item.calls:0}};
    }
    return output.dump();
}
std::string DocEngine::HealthReport() const {
    uint64_t errors=0,stopped=0,versions=0;
    const bool available=txn_db_ && txn_db_->GetIntProperty("rocksdb.background-errors",&errors) &&
        txn_db_->GetIntProperty("rocksdb.is-write-stopped",&stopped) && txn_db_->GetIntProperty("rocksdb.num-live-versions",&versions);
    std::lock_guard lock(operations_.mutex);
    return nlohmann::json({{"ready",available && !errors && !stopped && !operations_.corruption && !operations_.io_error && !operations_.quiescing},
        {"properties_available",available},{"background_errors",errors},{"write_stopped",bool(stopped)},
        {"corruption_latched",operations_.corruption},{"io_error_latched",operations_.io_error},{"quiescing",operations_.quiescing},
        {"scope","observed state; not a full integrity scan"}}).dump();
}
DBResult DocEngine::PrepareShutdown() {
    {std::lock_guard lock(operations_.mutex); operations_.quiescing=true;}
    PauseDocumentMigration(); // Caller quiesces requests/transactions before entry.
    const auto health=nlohmann::json::parse(HealthReport());
    if(health["io_error_latched"]==true || health["corruption_latched"]==true || health["background_errors"]!=0)
        return DBResult::Err("storage degraded; refusing implicit WAL retry; use recovery runbook","io_error");
    const auto flushed=FlushStorage(); if(!flushed.success) return flushed;
    const auto status=txn_db_->SyncWAL();
    if(!status.ok()) {std::lock_guard lock(operations_.mutex);operations_.io_error=true;return DBResult::Err(status.ToString(),"io_error");}
    return DBResult::Ok("{\"flushed\":true,\"wal_synced\":true}");
}
}
