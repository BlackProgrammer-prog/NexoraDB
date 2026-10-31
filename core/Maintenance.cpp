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
#include <charconv>
#include <rocksdb/slice_transform.h>
namespace nexora::core {
namespace {
using Json=nlohmann::json;
constexpr const char* manifest_key="meta:format:components";
Json supported() { return {{"manifest",1},{"min_reader",3},{"document_keys",1},
    {"document_envelope",1},{"indexes",3},{"graph_storage",2},{"graph_wal",2}}; }
Json legacy_supported() {auto value=supported();value["indexes"]=2;value["min_reader"]=2;return value;}
void require(const rocksdb::Status& status) { if(!status.ok()) throw std::runtime_error(status.ToString()); }
struct Epoch {
    rocksdb::DB* db; const rocksdb::Snapshot* snapshot;
    explicit Epoch(rocksdb::DB* value):db(value),snapshot(db->GetSnapshot()) {
        if(!snapshot) throw std::runtime_error("unable to acquire maintenance snapshot");
    }
    ~Epoch() { db->ReleaseSnapshot(snapshot); }
};
template<class Fn> void scan(rocksdb::DB* db,rocksdb::ReadOptions options,const std::string& prefix,Fn fn) {
    if(prefix.empty()) {
        options=MakeScanReadOptions(options);
        std::unique_ptr<rocksdb::Iterator> iterator(db->NewIterator(options));
        for(iterator->SeekToFirst();iterator->Valid();iterator->Next()) fn(iterator->key().ToString(),iterator->value().ToString());
        require(iterator->status());return;
    }
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
        if(store=="2" || store=="3") throw std::runtime_error("fenced generation requires component manifest");
        return;
    }
    require(status);
    const auto actual=Json::parse(bytes),known=supported();
    if(actual==legacy_supported() && store=="2")
        throw std::runtime_error("Legacy index format requires offline copy migration to generation 3");
    if(actual!=known) throw std::runtime_error("unsupported or incomplete storage component versions");
    if(store!="3") throw std::runtime_error("component manifest requires generation-3 reader fence; use copy migration");
}
rocksdb::Status DocEngine::EnsureIndexFormatFence() {
    std::lock_guard migration(document_migration_mutex_);
    rocksdb::WriteBatch batch;
    batch.Put(manifest_key,supported().dump());batch.Put("meta:format:documents","3");
    auto options=write_options_;options.sync=true;
    return txn_db_->Write(options,&batch);
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
            ++indexes; legacy_indexes+=index->format_version!=indexv2::kFormatVersion;
        });
        std::string format,manifest;
        auto status=db->Get(read,"meta:format:documents",&format);
        if(!status.IsNotFound()) require(status);
        if(!format.empty() && format!="1" && format!="2" && format!="3") throw std::runtime_error("unknown document store generation");
        status=db->Get(read,manifest_key,&manifest);
        if(!status.IsNotFound()) { require(status);const auto actual=Json::parse(manifest);
            if(!((actual==supported() && format=="3") || (actual==legacy_supported() && format=="2")))
                throw std::runtime_error("unknown component versions"); }
        else if(format=="2" || format=="3") throw std::runtime_error("missing component manifest");
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
        if(format!="1" && format!="3") throw std::runtime_error("finish document migration first");
        rocksdb::WriteBatch batch;
        batch.Put(manifest_key,supported().dump());
        // Pre-phase-8 binaries reject this existing marker before initialization.
        // Physical document envelopes stay NXD1; this is the store reader fence.
        batch.Put("meta:format:documents","3");
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
DBResult DocEngine::MigrateIndexFormatCopy(const std::string& source,const std::string& destination,
        bool dry_run,const std::function<void(uint64_t)>& progress) {
    // Only this freshly-created scratch directory is ever removed. Apply
    // failures retain their incomplete destination and never modify source.
    struct Scratch {
        std::filesystem::path path;
        bool owned=false;
        ~Scratch() {if(owned) {std::error_code error;std::filesystem::remove_all(path,error);}}
    } scratch;
    try {
        if(dry_run && !destination.empty())
            throw std::runtime_error("dry-run does not accept destination; use TMPDIR to choose scratch volume");
        const auto origin=std::filesystem::canonical(source);
        if(!std::filesystem::is_regular_file(origin/"CURRENT")) throw std::runtime_error("source is not a RocksDB store");
        auto* env=rocksdb::Env::Default();rocksdb::FileLock* raw_lock=nullptr;
        require(env->LockFile((origin/"LOCK").string(),&raw_lock));
        const auto unlock=[env](rocksdb::FileLock* lock){if(lock) (void)env->UnlockFile(lock);};
        std::unique_ptr<rocksdb::FileLock,decltype(unlock)> source_lock(raw_lock,unlock);
        RocksSettings settings;settings.memory_budget_bytes=32*1024*1024;
        auto resources=std::make_shared<RocksResources>(settings.memory_budget_bytes);
        auto options=MakeRocksOptions(settings,resources);options.create_if_missing=false;
        struct QuietSourceLogger final:rocksdb::Logger {void Logv(const char*,va_list) override {}};
        options.info_log=std::make_shared<QuietSourceLogger>();
        rocksdb::DB* raw=nullptr;require(rocksdb::DB::OpenForReadOnly(options,origin.string(),&raw));
        std::unique_ptr<rocksdb::DB> input(raw);require(input->VerifyChecksum());
        Epoch epoch(input.get());auto read=MakeDocumentReadOptions();read.snapshot=epoch.snapshot;
        std::string generation,manifest;
        auto status=input->Get(read,"meta:format:documents",&generation);
        if(!status.IsNotFound()) require(status);
        if(!generation.empty() && generation!="1" && generation!="2" && generation!="3")
            throw std::runtime_error("unknown/incomplete source generation");
        status=input->Get(read,manifest_key,&manifest);
        if(!status.IsNotFound()) {
            require(status);const auto value=Json::parse(manifest);
            if(!((generation=="2" && value==legacy_supported()) || (generation=="3" && value==supported())))
                throw std::runtime_error("unknown source component versions");
        } else if(generation=="2" || generation=="3") throw std::runtime_error("source manifest missing");

        const auto target=dry_run ? std::filesystem::temp_directory_path()/("nexora-index-audit-"+GenerateDocId()) :
            std::filesystem::weakly_canonical(std::filesystem::absolute(destination));
        if(!dry_run && destination.empty()) throw std::runtime_error("new destination required");
        const auto relative=target.lexically_relative(origin);
        if(relative.empty() || *relative.begin()!="..") throw std::runtime_error("destination must be outside source");
        if(!std::filesystem::create_directory(target)) throw std::runtime_error("destination exists; refusing overwrite");
        if(dry_run) {scratch.path=target;scratch.owned=true;}
        std::filesystem::permissions(target,std::filesystem::perms::owner_all,std::filesystem::perm_options::replace);
        auto target_resources=std::make_shared<RocksResources>(settings.memory_budget_bytes);
        auto target_options=MakeRocksOptions(settings,target_resources);
        rocksdb::DB* raw_target=nullptr;require(rocksdb::DB::Open(target_options,target.string(),&raw_target));
        std::unique_ptr<rocksdb::DB> output(raw_target);
        auto synced=MakeDocumentWriteOptions(DocumentDurability::Durable);
        auto staging=MakeDocumentWriteOptions(DocumentDurability::Buffered);
        require(output->Put(synced,"meta:format:documents","3-incomplete"));
        // Preserve opaque auth/schema/FK metadata and counters, but never copy
        // old physical indexes or reader fences into a candidate store.
        rocksdb::WriteBatch metadata;size_t metadata_bytes=0;
        const auto flush_metadata=[&] {if(metadata.Count()) {require(output->Write(staging,&metadata));metadata.Clear();metadata_bytes=0;}};
        scan(input.get(),read,"",[&](const std::string& key,const std::string& value) {
            if(key.starts_with("data:") || key.starts_with("idx:") || key.starts_with("idx2") ||
               key.starts_with("idx3") || key.starts_with("meta:idx:") || key==manifest_key ||
               key=="meta:format:documents" || key=="meta:migration:documents:v1") return;
            metadata.Put(key,value);metadata_bytes+=key.size()+value.size();
            if(metadata_bytes>=1024*1024) flush_metadata();
        });
        flush_metadata();
        uint64_t documents=0,indexes=0,entries=0;
        std::unordered_set<std::string> identities;
        scan(input.get(),read,"meta:col:",[&](const std::string& key,const std::string& schema) {
            const auto parsed_schema=DeserializeSchema(schema);
            if(!parsed_schema) throw std::runtime_error("invalid collection schema");
            for(const auto& field:parsed_schema->fields)
                if(unsigned(field.type)>unsigned(FieldType::Null)) throw std::runtime_error("unknown schema field type");
            const auto collection=key.substr(9),prefix="data:"+collection+":";
            std::vector<IndexDefinition> definitions;
            scan(input.get(),read,"meta:idx:"+collection+":",[&](const std::string& physical,const std::string& bytes) {
                auto index=DeserializeIndex(bytes);
                if(!index || index->fields.empty() || index->state!=IndexState::Ready ||
                   unsigned(index->type)>unsigned(IndexType::Unique) ||
                   (generation=="3" ? index->format_version!=3 : (index->format_version!=1 && index->format_version!=2)))
                    throw std::runtime_error("source index not ready/known; complete builds using original reader first");
                if(index->index_id.empty()) index->index_id=GenerateDocId();
                if(!identities.insert(indexv2::IndexPrefix(collection,index->index_id)).second)
                    throw std::runtime_error("duplicate index identity");
                index->format_version=3;index->build_cursor.clear();index->last_error.clear();
                require(output->Put(staging,physical,SerializeIndex(*index)));
                definitions.push_back(*index);++indexes;
            });
            uint64_t collection_count=0;
            scan(input.get(),read,prefix,[&](const std::string& physical,const std::string& bytes) {
                const auto json=DocumentCodec::DecodeToJson(bytes);
                if(!json.success) throw std::runtime_error(json.error);
                const auto encoded=DocumentCodec::Detect(bytes)==DocumentCodec::Format::BsonV1 ?
                    DocumentCodec::EncodeBson(std::string_view(bytes).substr(DocumentCodec::kEnvelopeSize)) :
                    DocumentCodec::EncodeJson(json.value);
                if(!encoded.success) throw std::runtime_error(encoded.error);
                const auto id=physical.substr(prefix.size());
                const auto decoded=Json::parse(json.value);
                if(!collection.starts_with("__nexora_") &&
                   (!decoded.contains("_id") || !decoded["_id"].is_string() || decoded["_id"].get<std::string>()!=id))
                    throw std::runtime_error("document identity/key mismatch");
                rocksdb::WriteBatch batch;batch.Put(physical,encoded.value);
                for(const auto& index:definitions) {
                    const auto tuple=indexv2::EncodeTuple(encoded.value,index.fields);
                    if(!tuple) continue;
                    if(index.type==IndexType::Unique) {
                        const auto unique=indexv2::UniqueKey(collection,index.index_id,*tuple);
                        std::string owner;const auto found=output->Get(MakeDocumentReadOptions(),unique,&owner);
                        if(found.ok() && owner!=id)
                            throw std::runtime_error("numeric/typed unique collision in "+collection+"/"+index.index_name+" between IDs "+owner+" and "+id);
                        if(!found.ok() && !found.IsNotFound()) require(found);
                        batch.Put(unique,id);
                    }
                    batch.Put(indexv2::EntryKey(collection,index.index_id,*tuple,id),id);++entries;
                }
                require(output->Write(staging,&batch));++documents;++collection_count;
                if(progress) progress(documents);
            });
            std::string counter;require(input->Get(read,"seq:"+collection,&counter));
            if(counter!=std::to_string(collection_count)) throw std::runtime_error("source counter mismatch: "+collection);
        });
        uint64_t source_documents=0;
        scan(input.get(),read,"data:",[&](const auto&,const auto&){++source_documents;});
        if(source_documents!=documents) throw std::runtime_error("unknown/overlapping collection data; refusing partial migration");
        uint64_t source_indexes=0;
        scan(input.get(),read,"meta:idx:",[&](const auto&,const auto&){++source_indexes;});
        if(source_indexes!=indexes) throw std::runtime_error("orphan/overlapping index metadata; refusing partial migration");
        rocksdb::FlushOptions flush;flush.wait=true;require(output->Flush(flush));require(output->VerifyChecksum());
        // Verify BEFORE publishing the complete reader fence. No native engine
        // can open this candidate while it is incomplete, even after a crash.
        auto target_read=MakeDocumentReadOptions();
        std::unordered_set<std::string> known_prefixes;
        scan(output.get(),target_read,"meta:col:",[&](const std::string& key,const auto&) {
            const auto collection=key.substr(9),data_prefix="data:"+collection+":";
            scan(output.get(),target_read,"meta:idx:"+collection+":",[&](const auto&,const std::string& bytes) {
                const auto index=DeserializeIndex(bytes);
                if(!index || index->format_version!=3) throw std::runtime_error("candidate index metadata invalid");
                const auto index_prefix=indexv2::IndexPrefix(collection,index->index_id);
                if(!known_prefixes.insert(index_prefix).second) throw std::runtime_error("candidate duplicate index identity");
                scan(output.get(),target_read,data_prefix,[&](const std::string& physical,const std::string& stored) {
                    const auto tuple=indexv2::EncodeTuple(stored,index->fields);if(!tuple) return;
                    const auto id=physical.substr(data_prefix.size());std::string owner;
                    require(output->Get(target_read,indexv2::EntryKey(collection,index->index_id,*tuple,id),&owner));
                    if(owner!=id) throw std::runtime_error("candidate entry owner mismatch");
                    if(index->type==IndexType::Unique) {
                        require(output->Get(target_read,indexv2::UniqueKey(collection,index->index_id,*tuple),&owner));
                        if(owner!=id) throw std::runtime_error("candidate unique owner mismatch");
                    }
                });
                scan(output.get(),target_read,index_prefix,[&](const std::string& physical,const std::string& id) {
                    std::string stored;require(output->Get(target_read,data_prefix+id,&stored));
                    const auto tuple=indexv2::EncodeTuple(stored,index->fields);
                    if(!tuple || (physical!=indexv2::EntryKey(collection,index->index_id,*tuple,id) &&
                        !(index->type==IndexType::Unique && physical==indexv2::UniqueKey(collection,index->index_id,*tuple))))
                        throw std::runtime_error("candidate stale index entry");
                });
            });
        });
        const auto extractor=MakeKeyspacePrefixExtractor();
        scan(output.get(),target_read,"idx3",[&](const std::string& key,const auto&) {
            if(!extractor->InDomain(key) || !known_prefixes.count(extractor->Transform(key).ToString()))
                throw std::runtime_error("candidate orphan index entry");
        });
        rocksdb::WriteBatch publish;publish.Put(manifest_key,supported().dump());publish.Put("meta:format:documents","3");
        require(output->Write(synced,&publish));
        output.reset();
        return DBResult::Ok(Json({{"dry_run",dry_run},{"source",origin.string()},
            {"destination",dry_run?Json(nullptr):Json(target.string())},{"documents",documents},{"indexes",indexes},
            {"index_entries",entries},{"format",3},{"source_unchanged",true},{"verified",true},
            {"external_graph_files_copied",false},{"activation","manual; source retained for rollback"}}).dump());
    } catch(const std::exception& error) {return DBResult::Err(error.what(),"migration_failed");}
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
        scan(txn_db_.get(),options,"meta:col:",[&](const std::string& key,const auto&) {
            const auto collection=key.substr(9);
            uint64_t actual=0;
            scan(txn_db_.get(),options,"data:"+collection+":",[&](const auto&,const auto&) { ++actual; });
            std::string counter;
            require(txn_db_->Get(options,"seq:"+collection,&counter));
            int64_t stored=-1;
            const auto parsed=std::from_chars(counter.data(),counter.data()+counter.size(),stored);
            if(parsed.ec!=std::errc{} || parsed.ptr!=counter.data()+counter.size() || stored<0 ||
               static_cast<uint64_t>(stored)!=actual)
                throw std::runtime_error("invalid or inconsistent document counter: "+collection);
        });
        if(verify_indexes) {
            // Metadata and every expected/actual entry share one read epoch.
            scan(txn_db_.get(),options,"meta:col:",[&](const std::string& key,const auto&) {
                const auto collection=key.substr(9),data_prefix="data:"+collection+":";
                scan(txn_db_.get(),options,"meta:idx:"+collection+":",[&](const auto&,const std::string& metadata) {
                    const auto index=DeserializeIndex(metadata);
                    if(!index || index->format_version!=indexv2::kFormatVersion || index->index_id.empty() ||
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
            scan(txn_db_.get(),options,"idx3",[&](const std::string& key,const auto&) {
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
