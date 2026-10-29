#include "DocEngine.h"
#include "DocumentCodec.h"
#include "IndexCodec.h"
#include "query/DocumentView.h"
#include <charconv>
#include <unordered_set>
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
    std::vector<std::string_view> views(docs.begin(),docs.end());
    return Observe(OperationsState::BatchInsert,[&]{return InsertManyImpl(collection,views);});
}
DBResult DocEngine::InsertManyBson(const std::string& collection,const std::vector<std::string>& docs) {
    return InsertManyBsonViews(collection,std::vector<std::string_view>(docs.begin(),docs.end()));
}
DBResult DocEngine::InsertManyBsonViews(const std::string& collection,const std::vector<std::string_view>& docs) {
    return Observe(OperationsState::BatchInsert,[&]{return InsertManyImpl(collection,docs,true);});
}
DBResult DocEngine::InsertManyBsonIngest(const std::string& collection,const std::vector<std::string_view>& docs) {
    return Observe(OperationsState::BatchInsert,[&]() -> DBResult {
        const auto RocksFailure=[](const std::string& operation,const rocksdb::Status& status) {
            return DBResult::Err("[RocksDB] "+operation+": "+status.ToString(),
                status.IsCorruption()?"corruption":status.IsIOError()?"io_error":"operation_failed");
        };
        TouchUserActivity();
        if(docs.empty() || docs.size()>10000) return DBResult::Err("Ingestion batch requires 1..10000 documents");
        size_t bytes=0;
        for(const auto doc:docs) {
            if(doc.size()>64*1024*1024-bytes) return DBResult::Err("Ingestion batch exceeds 64MiB");
            bytes+=doc.size();
        }
        std::unique_lock ddl(index_ddl_mutex_,std::defer_lock);
        {WriteStageTimer timer(operations_,OperationsState::DdlWaitStage);ddl.lock();}
        if(IsReservedCollectionName(collection)) return DBResult::Err("reserved collection name");
        MutationMetadata metadata;
        auto status=LoadMutationMetadata(collection,metadata);
        if(!status.ok()) return RocksFailure("Ingest metadata",status);
        const auto fallback=[&] {
            ddl.unlock();
            auto result=InsertManyImpl(collection,docs,true);
            if(result.success) result.data=std::to_string(docs.size());
            return result;
        };
        if(metadata.schema.strict || !metadata.schema.fields.empty() || !metadata.foreign_keys.empty()) return fallback();
        for(const auto& index:metadata.indexes) {
            if(index.type==IndexType::Unique || index.state!=IndexState::Ready ||
               index.format_version!=indexv2::kFormatVersion || index.index_id.empty()) return fallback();
        }
        struct Index { std::string prefix; std::vector<query::CompiledFieldPath> fields; };
        std::vector<Index> indexes;
        for(const auto& source:metadata.indexes) {
            Index index{indexv2::IndexPrefix(collection,source.index_id),{}};
            for(const auto& field:source.fields) index.fields.emplace_back(field);
            indexes.push_back(std::move(index));
        }
        std::vector<std::string> encoded,ids,keys;
        encoded.reserve(docs.size());ids.reserve(docs.size());keys.reserve(docs.size());
        for(const auto doc:docs) {
            DocumentCodec::BsonIdentity id;
            auto validated=DocumentCodec::EncodeBson(doc,&id);
            if(!validated.success) return DBResult::Err(validated.error,"validation_failed");
            encoded.push_back(std::move(validated.value));
            if(!id.found) return fallback();
            if(!id.is_string || id.value.empty() || id.value.size()>1024)
                return DBResult::Err("Document _id must be a nonempty string of at most 1 KiB","validation_failed");
            ids.emplace_back(id.value);
            keys.push_back(MakeDocKey(collection,ids.back()));
        }
        if(std::is_sorted(ids.begin(),ids.end())) {
            if(std::adjacent_find(ids.begin(),ids.end())!=ids.end())
                return DBResult::Err("Duplicate document _id in ingestion batch");
        } else {
            std::unordered_set<std::string_view> seen;seen.reserve(ids.size());
            for(const auto& id:ids) if(!seen.emplace(id).second)
                return DBResult::Err("Duplicate document _id in ingestion batch");
        }
        // Same exclusivity proof used by DropCollection: every foreground
        // mutation/TxHandle holds the shared DDL gate. Migration is separate.
        // Never use this bypass in the ordinary concurrent transaction path.
        std::lock_guard migration(document_migration_mutex_);
        bool range_proves_absent=false;
        {
            // A fresh, bounded primary-key range fence (never a cached max-ID).
            // Under both mutation gates, min(new keys)>max(existing keys)
            // proves absence for the whole batch. Random/backfill keys still
            // take exact MultiGet checks. Do not infer absence from COUNT.
            const auto prefix=MakeDocKey(collection,"");
            auto upper_value=prefix; ++upper_value.back(); // final ':' -> ';'
            const rocksdb::Slice lower(prefix),upper(upper_value);
            auto options=MakeScanReadOptions(read_options_);
            options.iterate_lower_bound=&lower;options.iterate_upper_bound=&upper;
            auto iterator=std::unique_ptr<rocksdb::Iterator>(txn_db_->NewIterator(options));
            iterator->SeekToLast();
            if(!iterator->status().ok()) return RocksFailure("Ingest primary range fence",iterator->status());
            if(!iterator->Valid()) range_proves_absent=true;
            else if(iterator->key().starts_with(prefix)) {
                const auto minimum=std::min_element(keys.begin(),keys.end());
                range_proves_absent=rocksdb::Slice(*minimum).compare(iterator->key())>0;
            }
        }
        if(!range_proves_absent) {
            std::vector<rocksdb::Slice> slices;slices.reserve(keys.size());
            for(const auto& key:keys) slices.emplace_back(key);
            std::vector<std::string> previous;
            const auto statuses=txn_db_->MultiGet(read_options_,slices,&previous);
            for(const auto& found:statuses) {
                if(found.ok()) return DBResult::Err("Duplicate document _id in ingestion store");
                if(!found.IsNotFound()) return RocksFailure("Ingest existence check",found);
            }
        }
        std::string stored_count;
        const std::string counter="seq:"+collection;
        status=txn_db_->Get(read_options_,counter,&stored_count);
        if(!status.ok()) return RocksFailure("Ingest counter",status);
        int64_t count=0;
        const auto parsed=std::from_chars(stored_count.data(),stored_count.data()+stored_count.size(),count);
        if(parsed.ec!=std::errc{} || parsed.ptr!=stored_count.data()+stored_count.size() || count<0 ||
           uint64_t(count)>uint64_t(std::numeric_limits<int64_t>::max())-docs.size())
            return DBResult::Err("Invalid or overflowing ingestion counter","corruption");
        rocksdb::WriteBatch batch(bytes+docs.size()*128);
        // Keep primary-key writes together, then each index keyspace. Avoid
        // bouncing the memtable insertion path between data/index per row.
        for(size_t i=0;i<docs.size();++i) {
            status=batch.Put(keys[i],encoded[i]);
            if(!status.ok()) return RocksFailure("Ingest document staging",status);
            status=InjectMutationFaultIfRequested(MutationFaultPoint::AfterDocument);
            if(!status.ok()) return RocksFailure("Ingest document fault",status);
        }
        for(const auto& index:indexes) {
            for(size_t i=0;i<docs.size();++i) {
                query::DocumentView view{std::string_view(encoded[i]).substr(DocumentCodec::kEnvelopeSize)};
                const auto tuple=indexv2::EncodeTuple(view,index.fields);
                if(tuple) {
                    status=batch.Put(indexv2::EntryKeyFromPrefix(index.prefix,*tuple,ids[i]),ids[i]);
                    if(!status.ok()) return RocksFailure("Ingest index staging",status);
                }
            }
        }
        status=InjectMutationFaultIfRequested(MutationFaultPoint::AfterIndexes);
        if(!status.ok()) return RocksFailure("Ingest index fault",status);
        status=batch.Put(counter,std::to_string(count+int64_t(docs.size())));
        if(!status.ok()) return RocksFailure("Ingest counter staging",status);
        status=InjectMutationFaultIfRequested(MutationFaultPoint::AfterCounter);
        if(!status.ok()) return RocksFailure("Ingest counter fault",status);
        rocksdb::TransactionDBWriteOptimizations exclusive;
        exclusive.skip_concurrency_control=true;
        {WriteStageTimer timer(operations_,OperationsState::CommitStage);
         status=txn_db_->Write(write_options_,exclusive,&batch);}
        if(!status.ok()) return RocksFailure("Ingest atomic commit",status);
        return DBResult::Ok(std::to_string(ids.size()));
    });
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
    output["write_path"]={{"durable_ack",write_options_.sync},{"wal_enabled",!write_options_.disableWAL},
        {"unordered_write",db_options_.unordered_write},{"pipelined_write",db_options_.enable_pipelined_write},
        {"write_buffer_bytes",db_options_.write_buffer_size},{"max_background_jobs",db_options_.max_background_jobs},
        {"statistics_available",bool(db_options_.statistics)}};
    auto& stages=output["write_path"]["stages"];
    const char* stage_names[]={"commit","counter_get_for_update","insert_ddl_lock_wait"};
    for(size_t i=0;i<OperationsState::WriteStageCount;++i) {
        const auto& stage=operations_.write_stages[i];
        stages[stage_names[i]]={{"calls",stage.calls.load(std::memory_order_relaxed)},
            {"total_ns",stage.nanoseconds.load(std::memory_order_relaxed)},
            {"max_ns",stage.max_nanoseconds.load(std::memory_order_relaxed)}};
    }
    output["write_path"]["stage_scope"]="sampled overlapping wall times, not additive; counter includes lock/storage lookup, commit includes queue/WAL/memtable; savepoints excluded";
    if(const auto& stats=db_options_.statistics) {
        auto& path=output["write_path"];
        path["wal_syncs"]=stats->getTickerCount(rocksdb::WAL_FILE_SYNCED);
        path["wal_bytes"]=stats->getTickerCount(rocksdb::WAL_FILE_BYTES);
        path["writes_by_leader"]=stats->getTickerCount(rocksdb::WRITE_DONE_BY_SELF);
        path["writes_by_follower"]=stats->getTickerCount(rocksdb::WRITE_DONE_BY_OTHER);
        const auto histogram=[&](uint32_t kind) {
            rocksdb::HistogramData value{};
            stats->histogramData(kind,&value);
            return nlohmann::json{{"count",value.count},{"sum_us",value.sum},
                {"average_us",value.average},{"p50_us",value.median},
                {"p95_us",value.percentile95},{"p99_us",value.percentile99},{"max_us",value.max}};
        };
        path["wal_sync_latency"]=histogram(rocksdb::WAL_FILE_SYNC_MICROS);
        path["db_write_latency"]=histogram(rocksdb::DB_WRITE);
        path["db_get_latency"]=histogram(rocksdb::DB_GET);
    }
    return output.dump();
}
void DocEngine::ConfigureQueryTuning(bool admission,bool indexes) noexcept {
    query_tuning_.store(uint8_t((admission?1:0)|(indexes?2:0)),std::memory_order_relaxed);
}
std::string DocEngine::QueryTuning() const {
    const auto flags=query_tuning_.load(std::memory_order_relaxed);
    return nlohmann::json({{"read_cache_admission",bool(flags&1)},{"smart_indexes",bool(flags&2)},
        {"scope","this engine, runtime only; existing cache hits/metadata caching remain possible"},
        {"index_constraints_always_enforced",true}}).dump();
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
std::string DocEngine::DurabilityReport() const {
    return nlohmann::json({{"profile",write_options_.sync ? "durable" : "buffered"},
        {"wal_enabled",!write_options_.disableWAL},{"sync_on_ack",write_options_.sync},
        {"manual_wal_flush",db_options_.manual_wal_flush},
        {"ack_boundary","successful native write/transaction commit; transaction staging is not commit"},
        {"failure_outcome","unknown on IO/commit failure; reconcile before retry"},
        {"scope","document engine only; no replication or atomic document+graph transaction"},
        {"storage_assumption","filesystem/device honors successful sync; process-crash tests do not prove power-loss durability"}}).dump();
}
std::string DocEngine::MemoryReport() {
    nlohmann::json output;
    const auto metrics=GetRocksMetrics();
    output["cache_capacity_bytes"]=metrics.cache_capacity;
    output["cache_usage_bytes"]=metrics.cache_usage;
    output["cache_pinned_bytes"]=metrics.cache_pinned;
    output["memtable_budget_bytes"]=metrics.memtable_budget;
    output["memtable_usage_bytes"]=metrics.memtable_usage;
    const auto property=[&](const char* name,const char* label) {
        uint64_t value=0;
        if(txn_db_->GetIntProperty(name,&value)) output[label]=value;
        else output[label]=nullptr;
    };
    property("rocksdb.estimate-table-readers-mem","table_readers_estimate_bytes");
    property("rocksdb.cur-size-all-mem-tables","all_memtables_bytes");
    property("rocksdb.size-all-mem-tables","retained_memtables_bytes");
    property("rocksdb.num-snapshots","rocksdb_snapshots");
    property("rocksdb.oldest-snapshot-time","oldest_snapshot_unix_seconds");
    {
        std::lock_guard lock(page_sessions_mutex_);
        PrunePageSessionsLocked();
        const auto now=PageSessionNow();
        output["page_sessions"]=page_sessions_.size();
        output["page_session_capacity"]=pagination::kMaxSnapshots;
        const auto live=page_live_snapshots_->load();
        output["page_live_snapshots"]=live;
        output["page_retired_inflight_snapshots"]=live-page_sessions_.size();
        std::size_t active=0;
        double oldest_age=0;
        for (const auto& [id,session] : page_sessions_) {
            if (session.use_count()>1) ++active;
            oldest_age=std::max(oldest_age,std::chrono::duration<double>(now-session->created_at).count());
        }
        output["page_active_sessions"]=active;
        output["page_idle_sessions"]=page_sessions_.size()-active;
        output["page_oldest_session_age_seconds"]=oldest_age;
        output["page_idle_lease_seconds"]=pagination::kIdleLeaseSeconds;
        output["page_absolute_lifetime_seconds"]=nullptr; // Renewable active scans have no absolute cutoff.
        output["page_snapshot_bytes"]=nullptr; // RocksDB exposes no exact per-snapshot byte attribution.
    }
    output["scope"]="overlapping RocksDB estimates; cache includes index/filter blocks; excludes Python, allocator overhead and graph";
    return output.dump();
}
DBResult DocEngine::PrepareShutdown() {
    {std::lock_guard lock(operations_.mutex); operations_.quiescing=true;}
    PauseDocumentMigration(); // Caller quiesces requests/transactions before entry.
    page_reaper_worker_.request_stop();
    if(page_reaper_worker_.joinable()) page_reaper_worker_.join();
    // Release registry-owned snapshots before flushing. In-flight users retain
    // their shared ownership; callers must drain requests before shutdown.
    {std::lock_guard lock(page_sessions_mutex_);page_sessions_.clear();}
    rocks_resources_->cache->EraseUnRefEntries();
    const auto health=nlohmann::json::parse(HealthReport());
    if(health["io_error_latched"]==true || health["corruption_latched"]==true || health["background_errors"]!=0)
        return DBResult::Err("storage degraded; refusing implicit WAL retry; use recovery runbook","io_error");
    const auto flushed=FlushStorage(); if(!flushed.success) return flushed;
    const auto status=txn_db_->SyncWAL();
    if(!status.ok()) {std::lock_guard lock(operations_.mutex);operations_.io_error=true;return DBResult::Err(status.ToString(),"io_error");}
    return DBResult::Ok("{\"flushed\":true,\"wal_synced\":true}");
}
}
