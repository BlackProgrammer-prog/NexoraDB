#include "DocEngine.h"
#include "DocumentCodec.h"
#include "IndexCodec.h"
#include <rocksdb/utilities/checkpoint.h>
#include <rocksdb/write_batch.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <stdexcept>
#include <cstdarg>
#include <unordered_set>
#include <rocksdb/slice_transform.h>
namespace nexora::core {
namespace {
using Json=nlohmann::json;
constexpr const char* manifest_key="meta:format:components";
Json supported() { return {{"manifest",1},{"min_reader",2},{"document_keys",1},
    {"document_envelope",1},{"indexes",2},{"graph_storage",2},{"graph_wal",2}}; }
void require(const rocksdb::Status& status) { if(!status.ok()) throw std::runtime_error(status.ToString()); }
struct Epoch {
    rocksdb::DB* db; const rocksdb::Snapshot* snapshot;
    explicit Epoch(rocksdb::DB* value):db(value),snapshot(db->GetSnapshot()) {
        if(!snapshot) throw std::runtime_error("unable to acquire maintenance snapshot");
    }
    ~Epoch() { db->ReleaseSnapshot(snapshot); }
};
template<class Fn> void scan(rocksdb::DB* db,rocksdb::ReadOptions options,const std::string& prefix,Fn fn) {
    std::string upper=prefix;
    size_t end=upper.size();
    while(end && static_cast<unsigned char>(upper[end-1])==255) --end;
    if(!end) throw std::runtime_error("invalid maintenance prefix");
    upper.resize(end); upper.back()=char(static_cast<unsigned char>(upper.back())+1);
    rocksdb::Slice bound(upper); options.iterate_upper_bound=&bound;
    options=MakeScanReadOptions(options);
    std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(options));
    for(it->Seek(prefix);it->Valid();it->Next()) fn(it->key().ToString(),it->value().ToString());
    require(it->status());
}
}
void DocEngine::ValidateFormatManifest() const {
    std::string bytes,store;
    const auto status=txn_db_->Get(read_options_,manifest_key,&bytes);
    const auto store_status=txn_db_->Get(read_options_,"meta:format:documents",&store);
    if(!store_status.IsNotFound()) require(store_status);
    if(status.IsNotFound()) {
        if(store=="2") throw std::runtime_error("format generation 2 requires component manifest");
        return;
    }
    require(status);
    const auto actual=Json::parse(bytes),known=supported();
    if(actual!=known) throw std::runtime_error("unsupported or incomplete storage component versions");
    if(store!="2") throw std::runtime_error("component manifest requires generation-2 reader fence");
}
DBResult DocEngine::FormatVersions() const {
    try {
        std::shared_lock ddl(index_ddl_mutex_);
        ValidateFormatManifest(); std::string bytes;
        const auto status=txn_db_->Get(read_options_,manifest_key,&bytes);
        if(status.IsNotFound()) return DBResult::Ok(Json({{"installed",false},{"supported",supported()}}).dump());
        require(status);
        return DBResult::Ok(Json({{"installed",true},{"versions",Json::parse(bytes)}}).dump());
    } catch(const std::exception& error) { return DBResult::Err(error.what()); }
}
void DocEngine::PauseDocumentMigration() {
    std::unique_lock ddl(index_ddl_mutex_);
    document_migration_worker_.request_stop();
    if(document_migration_worker_.joinable()) document_migration_worker_.join();
}
DBResult DocEngine::InspectStore(const std::string& path) {
    try {
        struct QuietLogger final:rocksdb::Logger { void Logv(const char*,va_list) override {} };
        RocksSettings settings; auto resources=std::make_shared<RocksResources>(32*1024*1024);
        auto options=MakeRocksOptions(settings,resources); options.create_if_missing=false;
        options.info_log=std::make_shared<QuietLogger>();
        rocksdb::DB* raw=nullptr; require(rocksdb::DB::OpenForReadOnly(options,path,&raw));
        std::unique_ptr<rocksdb::DB> db(raw); Epoch epoch(db.get()); auto read=MakeDocumentReadOptions(); read.snapshot=epoch.snapshot;
        uint64_t documents=0,legacy=0,bytes=0,indexes=0,legacy_indexes=0;
        scan(db.get(),read,"data:",[&](const auto&,const std::string& stored) {
            const auto decoded=DocumentCodec::DecodeToJson(stored);
            if(!decoded.success) throw std::runtime_error(decoded.error);
            ++documents; bytes+=stored.size(); legacy+=DocumentCodec::Detect(stored)==DocumentCodec::Format::LegacyJson;
        });
        scan(db.get(),read,"meta:idx:",[&](const auto&,const std::string& metadata) {
            const auto index=DeserializeIndex(metadata);
            if(!index) throw std::runtime_error("invalid index metadata");
            ++indexes; legacy_indexes+=index->format_version!=2;
        });
        std::string format,manifest;
        auto status=db->Get(read,"meta:format:documents",&format);
        if(!status.IsNotFound()) require(status);
        if(!format.empty() && format!="1" && format!="2") throw std::runtime_error("unknown document store generation");
        status=db->Get(read,manifest_key,&manifest);
        if(!status.IsNotFound()) { require(status); if(Json::parse(manifest)!=supported() || format!="2") throw std::runtime_error("unknown component versions"); }
        else if(format=="2") throw std::runtime_error("missing component manifest");
        return DBResult::Ok(Json({{"documents",documents},{"legacy_documents",legacy},{"stored_document_bytes",bytes},
            {"indexes",indexes},{"legacy_indexes",legacy_indexes},{"store_generation",format},
            {"supported",supported()}}).dump());
    } catch(const std::exception& error) { return DBResult::Err(error.what()); }
}
DBResult DocEngine::UpgradeFormatManifest() {
    try {
        const auto checked=IntegrityCheck();
        if(!checked.success) return checked;
        if(Json::parse(checked.data).at("legacy_documents")!=0)
            throw std::runtime_error("legacy documents remain; finish migration first");
        std::unique_lock ddl(index_ddl_mutex_);
        std::lock_guard migration(document_migration_mutex_);
        ValidateFormatManifest(); std::string format;
        require(txn_db_->Get(read_options_,"meta:format:documents",&format));
        if(format!="1" && format!="2") throw std::runtime_error("finish document migration first");
        rocksdb::WriteBatch batch;
        batch.Put(manifest_key,supported().dump());
        // Pre-phase-8 binaries reject this existing marker before initialization.
        // Physical document envelopes stay NXD1; this is the store reader fence.
        batch.Put("meta:format:documents","2");
        auto options=write_options_; options.sync=true;
        require(txn_db_->Write(options,&batch));
        return DBResult::Ok(supported().dump());
    } catch(const std::exception& error) { return DBResult::Err(error.what()); }
}
DBResult DocEngine::CreateCheckpoint(const std::string& destination) {
    try {
        const auto target=std::filesystem::absolute(destination).lexically_normal();
        const auto source=std::filesystem::weakly_canonical(db_path_);
        const auto resolved=std::filesystem::weakly_canonical(target);
        auto relative=resolved.lexically_relative(source);
        if(relative.empty() || (!relative.empty() && *relative.begin()!=".."))
            throw std::runtime_error("checkpoint must be outside source database");
        rocksdb::Checkpoint* raw=nullptr; require(rocksdb::Checkpoint::Create(txn_db_.get(),&raw));
        std::unique_ptr<rocksdb::Checkpoint> checkpoint(raw); uint64_t sequence=0;
        require(checkpoint->CreateCheckpoint(target.string(),0,&sequence));
        return DBResult::Ok(Json({{"path",target.string()},{"included_sequence_lower_bound",sequence}}).dump());
    } catch(const std::exception& error) { return DBResult::Err(error.what()); }
}
DBResult DocEngine::IntegrityCheck(bool verify_indexes) const {
    try {
        std::shared_lock ddl(index_ddl_mutex_);
        ValidateFormatManifest(); require(txn_db_->VerifyChecksum());
        Epoch epoch(txn_db_.get()); auto options=read_options_; options.snapshot=epoch.snapshot;
        uint64_t documents=0,legacy=0,index_entries=0,indexes=0;
        std::unordered_set<std::string> known_index_prefixes;
        scan(txn_db_.get(),options,"data:",[&](const auto&,const auto& bytes) {
            const auto decoded=DocumentCodec::DecodeToJson(bytes);
            if(!decoded.success) throw std::runtime_error("corrupt document: "+decoded.error);
            ++documents; legacy+=DocumentCodec::Detect(bytes)==DocumentCodec::Format::LegacyJson;
        });
        if(verify_indexes) {
            // Metadata and every expected/actual entry share one read epoch.
            scan(txn_db_.get(),options,"meta:col:",[&](const std::string& key,const auto&) {
                const auto collection=key.substr(9),data_prefix="data:"+collection+":";
                scan(txn_db_.get(),options,"meta:idx:"+collection+":",[&](const auto&,const std::string& metadata) {
                    const auto index=DeserializeIndex(metadata);
                    if(!index || index->format_version!=2 || index->index_id.empty() ||
                       index->state!=IndexState::Ready || unsigned(index->type)>unsigned(IndexType::Unique))
                        throw std::runtime_error("index metadata invalid/not ready; rebuild or migrate required");
                    ++indexes;
                    if(!known_index_prefixes.insert(indexv2::IndexPrefix(collection,index->index_id)).second)
                        throw std::runtime_error("duplicate physical index identity");
                    scan(txn_db_.get(),options,data_prefix,[&](const std::string& physical,const std::string& stored) {
                        const auto decoded=DocumentCodec::DecodeToJson(stored);
                        if(!decoded.success) throw std::runtime_error("corrupt indexed document");
                        const auto tuple=indexv2::EncodeTuple(decoded.value,index->fields);
                        if(!tuple) return;
                        const auto id=physical.substr(data_prefix.size()); std::string owner;
                        require(txn_db_->Get(options,indexv2::EntryKey(collection,index->index_id,*tuple,id),&owner));
                        if(owner!=id) throw std::runtime_error("index entry owner mismatch");
                        if(index->type==IndexType::Unique) {
                            require(txn_db_->Get(options,indexv2::UniqueKey(collection,index->index_id,*tuple),&owner));
                            if(owner!=id) throw std::runtime_error("unique index owner mismatch");
                        }
                    });
                    scan(txn_db_.get(),options,indexv2::IndexPrefix(collection,index->index_id),
                        [&](const std::string& physical,const std::string& id) {
                            std::string stored; require(txn_db_->Get(options,data_prefix+id,&stored));
                            const auto decoded=DocumentCodec::DecodeToJson(stored);
                            if(!decoded.success) throw std::runtime_error("corrupt index target");
                            const auto tuple=indexv2::EncodeTuple(decoded.value,index->fields);
                            if(!tuple || (physical!=indexv2::EntryKey(collection,index->index_id,*tuple,id) &&
                                !(index->type==IndexType::Unique && physical==indexv2::UniqueKey(collection,index->index_id,*tuple))))
                                throw std::runtime_error("stale or malformed index entry");
                            ++index_entries;
                        });
                });
            });
            const auto extractor=MakeKeyspacePrefixExtractor();
            scan(txn_db_.get(),options,"idx2",[&](const std::string& key,const auto&) {
                if(!extractor->InDomain(key) || !known_index_prefixes.count(extractor->Transform(key).ToString()))
                    throw std::runtime_error("orphan or malformed physical index key");
            });
        }
        return DBResult::Ok(Json({{"documents",documents},{"legacy_documents",legacy},
            {"indexes",indexes},{"index_entries",index_entries},{"indexes_checked",verify_indexes}}).dump());
    } catch(const std::exception& error) {
        {std::lock_guard lock(operations_.mutex); operations_.corruption=true;}
        return DBResult::Err(error.what(),"integrity_failed");
    }
}
}
