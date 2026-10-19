#include "Graphstorage.h"
#include "StorageCodec.h"
#include "DurableFile.h"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <set>
#include <stdexcept>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace nexora::graph {
namespace {
namespace codec=storage_codec;
using Json=nlohmann::json;
constexpr size_t max_metadata_bytes=16*1024*1024;
std::string readFile(const std::filesystem::path& path,size_t limit) {
    const auto size=std::filesystem::file_size(path);
    if (size>limit) throw std::runtime_error("graph file exceeds limit: "+path.string());
    std::ifstream file(path,std::ios::binary);
    if (!file) throw std::runtime_error("cannot open graph file: "+path.string());
    std::string result(size,'\0'); file.read(result.data(),result.size());
    if (size && !file) throw std::runtime_error("graph file read failed: "+path.string());
    return result;
}
uint64_t number(std::string_view name) {
    uint64_t value=0;
    const auto parsed=std::from_chars(name.data(),name.data()+name.size(),value);
    if (name.empty() || parsed.ec!=std::errc{} || parsed.ptr!=name.data()+name.size() ||
        std::to_string(value)!=name) throw std::runtime_error("invalid graph identity/type key");
    return value;
}
bool validState(uint64_t state) {
    return state==uint32_t(GraphState::Clean)||state==uint32_t(GraphState::Dirty)||
           state==uint32_t(GraphState::Building)||state==uint32_t(GraphState::Corrupted);
}
Json metaJson(const GraphMeta& meta) {
    return {{"version",uint32_t(meta.version)},{"kind",uint8_t(meta.kind)},
        {"state",uint32_t(meta.state)},{"node_count",uint64_t(meta.node_count)},
        {"edge_count",uint64_t(meta.edge_count)},{"active_nodes",uint64_t(meta.active_nodes)},
        {"active_edges",uint64_t(meta.active_edges)},{"last_node_id",uint64_t(meta.last_node_id)},
        {"last_edge_id",uint64_t(meta.last_edge_id)},{"created_at",int64_t(meta.created_at)},
        {"updated_at",int64_t(meta.updated_at)},{"wal_retention_secs",int64_t(meta.wal_retention_secs)},
        {"graph_name",std::string(meta.graph_name,strnlen(meta.graph_name,sizeof meta.graph_name))},
        {"filter_expr",std::string(meta.filter_expr,strnlen(meta.filter_expr,sizeof meta.filter_expr))}};
}
GraphMeta decodeMeta(const Json& json) {
    GraphMeta meta{};
    const char* unsigned_fields[]={"version","kind","state","node_count","edge_count",
        "active_nodes","active_edges","last_node_id","last_edge_id"};
    for (const char* field:unsigned_fields)
        if (!json.at(field).is_number_unsigned()) throw std::runtime_error("invalid graph metadata number");
    if (json.at("version")!=1 || json.at("kind").get<uint64_t>()>1 ||
        !validState(json.at("state").get<uint64_t>())) throw std::runtime_error("invalid graph metadata enum/version");
    meta.kind=static_cast<GraphKind>(json.at("kind").get<uint8_t>());
    meta.state=static_cast<GraphState>(json.at("state").get<uint32_t>());
    meta.node_count=json.at("node_count").get<uint64_t>(); meta.edge_count=json.at("edge_count").get<uint64_t>();
    meta.active_nodes=json.at("active_nodes").get<uint64_t>(); meta.active_edges=json.at("active_edges").get<uint64_t>();
    meta.last_node_id=json.at("last_node_id").get<uint64_t>(); meta.last_edge_id=json.at("last_edge_id").get<uint64_t>();
    for (const char* field:{"created_at","updated_at","wal_retention_secs"})
        if (!json.at(field).is_number_integer() || (json.at(field).is_number_unsigned() &&
            json.at(field).get<uint64_t>()>INT64_MAX)) throw std::runtime_error("invalid graph timestamp/retention");
    meta.created_at=json.at("created_at").get<int64_t>(); meta.updated_at=json.at("updated_at").get<int64_t>();
    meta.wal_retention_secs=json.at("wal_retention_secs").get<int64_t>();
    const auto name=json.at("graph_name").get<std::string>();
    const auto filter=json.at("filter_expr").get<std::string>();
    if (name.size()>sizeof(meta.graph_name) || filter.size()>sizeof(meta.filter_expr))
        throw std::runtime_error("oversized graph metadata string");
    std::memcpy(meta.graph_name,name.data(),name.size()); std::memcpy(meta.filter_expr,filter.data(),filter.size());
    return meta;
}
std::string metadataBytes(const Json& metadata,uint64_t generation) {
    const auto json=metadata.dump();
    if (json.size()>max_metadata_bytes) throw std::runtime_error("graph metadata limit exceeded");
    std::string body(8,'\0'); codec::put(body,0,json.size(),8); body+=json;
    const auto crc=codec::crc(body); const auto at=body.size(); body.resize(at+4);
    codec::put(body,at,crc,4); return codec::header(3,generation)+body;
}
Json parseMetadata(const std::filesystem::path& path,uint64_t generation) {
    const auto bytes=readFile(path,max_metadata_bytes+44);
    if (bytes.size()<44) throw std::runtime_error("truncated graph metadata");
    codec::checkHeader(std::string_view(bytes).substr(0,32),3,generation);
    const auto length=codec::get(bytes,32,8);
    if (length>max_metadata_bytes || bytes.size()!=44+length ||
        codec::crc(std::string_view(bytes).substr(32,8+length))!=codec::get(bytes,40+length,4))
        throw std::runtime_error("graph metadata length/checksum mismatch");
    auto result=Json::parse(bytes.begin()+40,bytes.end()-4);
    (void)decodeMeta(result.at("meta"));
    for (const char* field:{"identities","node_types","edge_types"}) {
        if (!result.at(field).is_object()) throw std::runtime_error("invalid graph identity/type dictionary");
        std::set<std::string> type_names;
        for (auto it=result.at(field).begin();it!=result.at(field).end();++it) {
            const auto id=number(it.key());
            if (!it.value().is_string()) throw std::runtime_error("invalid graph identity/type value");
            if (std::string_view(field)!="identities" &&
                (id>=kInvalidTypeId || !type_names.insert(it.value().get<std::string>()).second))
                throw std::runtime_error("invalid or duplicate graph type");
        }
    }
    return result;
}
std::string manifest(uint64_t generation) {
    std::string bytes(32,'\0'); bytes.replace(0,4,"NXM2");
    codec::put(bytes,4,2,4); codec::put(bytes,8,generation,8); codec::seal(bytes,28); return bytes;
}
uint64_t parseManifest(const std::filesystem::path& path) {
    auto bytes=readFile(path,32);
    if (bytes.size()!=32 || bytes.substr(0,4)!="NXM2" || codec::get(bytes,4,4)!=2 ||
        codec::get(bytes,16,8) || codec::get(bytes,24,4)) throw std::runtime_error("invalid graph manifest");
    codec::check(bytes,28); const auto generation=codec::get(bytes,8,8);
    if (!generation || generation==UINT64_MAX) throw std::runtime_error("invalid graph generation");
    return generation;
}
std::filesystem::path pathFor(const std::filesystem::path& dir,const std::string& name,
                              uint64_t generation,const char* suffix) {
    std::ostringstream file; file<<name<<".g"<<std::setfill('0')<<std::setw(20)<<generation<<suffix;
    return dir/file.str();
}
bool syncFile(const std::filesystem::path& path,bool directory=false) {
    const int fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC|(directory?O_DIRECTORY:0));
    if (fd<0) return false;
    const bool ok=(directory ? ::fsync(fd) : ::fdatasync(fd))==0;
    return ::close(fd)==0 && ok;
}
} // namespace

GraphStorage::GraphStorage(const std::filesystem::path& dir,const std::string& name):dir_(dir),name_(name) {}
GraphStorage::~GraphStorage() { close(); }
bool GraphStorage::fail(const std::string& message) { error_=message; failed_=true; return false; }
std::string GraphStorage::lastError() const { std::lock_guard lock(mutex_); return error_; }
void GraphStorage::selectPaths(uint64_t generation) {
    node_path_=pathFor(dir_,name_,generation,".nex"); edge_path_=pathFor(dir_,name_,generation,".nexr");
    meta_path_=pathFor(dir_,name_,generation,".nexl");
}
uint64_t GraphStorage::nextGeneration() const {
    if (generation_>=UINT64_MAX-1) throw std::runtime_error("graph generation exhausted");
    uint64_t next=generation_+1;
    while (std::filesystem::exists(pathFor(dir_,name_,next,".nex")) ||
           std::filesystem::exists(pathFor(dir_,name_,next,".nexr")) ||
           std::filesystem::exists(pathFor(dir_,name_,next,".nexl"))) {
        if (next==UINT64_MAX-1) throw std::runtime_error("graph generation exhausted");
        ++next;
    }
    return next;
}
bool GraphStorage::open() {
    std::lock_guard lock(mutex_);
    if (is_open_) return !failed_;
    error_.clear(); failed_=false;
    try {
        if (!detail::ValidGraphName(name_)) throw std::runtime_error("invalid graph storage name");
        std::filesystem::create_directories(dir_);
        lock_fd_=::open((dir_/(name_+".storage.lock")).c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);
        if (lock_fd_<0 || ::flock(lock_fd_,LOCK_EX|LOCK_NB)!=0) throw std::runtime_error("graph storage already owned or unavailable");
        if (!std::filesystem::exists(manifestPath())) {
            for (const auto& entry:std::filesystem::directory_iterator(dir_))
                if (entry.path().filename().string().starts_with(name_+".g"))
                    throw std::runtime_error("graph generations exist but manifest is missing; recovery required");
            for (const auto& path:{dir_/(name_+".nex"),dir_/(name_+".nexr"),dir_/"graph_meta.nexl"})
                if (std::filesystem::exists(path) && std::filesystem::file_size(path))
                    throw std::runtime_error("legacy raw graph storage requires backup and source rebuild");
            generation_=0; const auto next=nextGeneration();
            Json metadata={{"meta",metaJson(GraphMeta{})},{"identities",Json::object()},
                           {"node_types",Json::object()},{"edge_types",Json::object()}};
            stage(next,{}, {},metadata,false);
            if (!detail::AtomicWriteFile(manifestPath(),manifest(next))) throw std::runtime_error("initial graph manifest publication failed");
        }
        if (!activate(parseManifest(manifestPath()))) throw std::runtime_error(error_);
        is_open_=true; return true;
    } catch (const std::exception& error) {
        const std::string message=error.what(); close();
        return fail("graph storage "+(dir_/name_).string()+": "+message);
    }
}
void GraphStorage::close() {
    std::lock_guard lock(mutex_);
    if (node_file_.is_open()) node_file_.close();
    if (edge_file_.is_open()) edge_file_.close();
    node_file_.clear(); edge_file_.clear();
    if (lock_fd_>=0) { ::close(lock_fd_); lock_fd_=-1; }
    is_open_=false;
}
bool GraphStorage::activate(uint64_t generation) {
    if (node_file_.is_open()) node_file_.close();
    if (edge_file_.is_open()) edge_file_.close();
    node_file_.clear(); edge_file_.clear(); selectPaths(generation); generation_=generation;
    metadata_=parseMetadata(meta_path_,generation);
    for (auto pair:{std::pair<std::fstream*,std::filesystem::path>{&node_file_,node_path_},
                    {&edge_file_,edge_path_}}) {
        pair.first->open(pair.second,std::ios::in|std::ios::out|std::ios::binary);
        if (!*pair.first) return fail("missing/unreadable referenced graph file: "+pair.second.string());
    }
    return validate();
}
bool GraphStorage::validate() {
    for (uint32_t kind:{1U,2U}) {
        auto& file=kind==1?node_file_:edge_file_;
        const auto path=kind==1?node_path_:edge_path_;
        std::string header(32,'\0'); file.seekg(0); file.read(header.data(),header.size());
        if (!file) return fail("truncated graph file header: "+path.string());
        codec::checkHeader(header,kind,generation_);
        const auto size=std::filesystem::file_size(path);
        if (size<32 || (size-32)%40) return fail("incomplete graph record: "+path.string());
        if (kind==1) node_file_records_=(size-32)/40;
        else edge_file_records_=(size-32)/40;
    }
    std::vector<bool> active(node_file_records_,false);
    std::set<std::string> identities;
    for (uint64_t id=0;id<node_file_records_;++id) {
        auto node=readNode(id); active[id]=node->isActive();
        const auto key=std::to_string(id);
        if (active[id] && metadata_["identities"].contains(key) &&
            !identities.insert(metadata_["identities"][key].get<std::string>()).second)
            return fail("duplicate active external graph identity");
    }
    for (uint64_t id=0;id<edge_file_records_;++id) {
        auto edge=readEdge(id);
        if (edge->isActive() && (edge->src>=active.size() || edge->dst>=active.size() ||
            !active[edge->src] || !active[edge->dst])) return fail("active graph edge has invalid endpoint");
    }
    return true;
}
std::optional<NodeRecord> GraphStorage::readNode(DenseId id) const {
    std::lock_guard lock(mutex_);
    if (failed_) throw std::runtime_error(error_);
    if (!node_file_.is_open() || id>=node_file_records_) return std::nullopt;
    std::string bytes(40,'\0'); node_file_.clear(); node_file_.seekg(32+id*40);
    node_file_.read(bytes.data(),bytes.size());
    try {
        if (!node_file_) throw std::runtime_error("graph node read failed");
        return codec::decodeNode(bytes,id);
    } catch (const std::exception& error) { failed_=true; error_=node_path_.string()+":"+std::to_string(id)+": "+error.what(); throw std::runtime_error(error_); }
}
std::optional<EdgeRecord> GraphStorage::readEdge(EdgeId id) const {
    std::lock_guard lock(mutex_);
    if (failed_) throw std::runtime_error(error_);
    if (!edge_file_.is_open() || id>=edge_file_records_) return std::nullopt;
    std::string bytes(40,'\0'); edge_file_.clear(); edge_file_.seekg(32+id*40);
    edge_file_.read(bytes.data(),bytes.size());
    try {
        if (!edge_file_) throw std::runtime_error("graph edge read failed");
        return codec::decodeEdge(bytes,id);
    } catch (const std::exception& error) { failed_=true; error_=edge_path_.string()+":"+std::to_string(id)+": "+error.what(); throw std::runtime_error(error_); }
}
bool GraphStorage::writeNode(const NodeRecord& record) {
    std::lock_guard lock(mutex_);
    return writeNodeUnlocked(record,true);
}
bool GraphStorage::writeNodeUnlocked(const NodeRecord& record,bool flush) {
    if (!is_open_ || failed_ || record.dense_id>(uint64_t(INT64_MAX)-32)/40) return false;
    try {
        codec::flags(record.flags,FLAG_ACTIVE|FLAG_DELETED|FLAG_IMPLICIT);
        node_file_.clear();
        if (record.dense_id>node_file_records_) node_file_.seekp(32+node_file_records_*40);
        for (uint64_t id=node_file_records_;id<record.dense_id;++id) {
            NodeRecord hole{}; hole.dense_id=id; const auto bytes=codec::node(hole);
            node_file_.write(bytes.data(),bytes.size());
        }
        const auto bytes=codec::node(record);
        const auto position=std::streamoff(32+record.dense_id*40);
        if (node_file_.tellp()!=position) node_file_.seekp(position);
        node_file_.write(bytes.data(),bytes.size()); if (flush) node_file_.flush();
        if (!node_file_) return fail("graph node write failed");
        node_file_records_=std::max(node_file_records_.load(),record.dense_id+1); return true;
    } catch (const std::exception& error) { return fail(error.what()); }
}
bool GraphStorage::writeEdge(const EdgeRecord& record) {
    std::lock_guard lock(mutex_);
    return writeEdgeUnlocked(record,true);
}
bool GraphStorage::writeEdgeUnlocked(const EdgeRecord& record,bool flush) {
    if (!is_open_ || failed_ || record.edge_id>(uint64_t(INT64_MAX)-32)/40) return false;
    try {
        codec::flags(record.flags,FLAG_ACTIVE|FLAG_DELETED|FLAG_UNDIRECTED);
        edge_file_.clear();
        if (record.edge_id>edge_file_records_) edge_file_.seekp(32+edge_file_records_*40);
        for (uint64_t id=edge_file_records_;id<record.edge_id;++id) {
            EdgeRecord hole{}; hole.edge_id=id; const auto bytes=codec::edge(hole);
            edge_file_.write(bytes.data(),bytes.size());
        }
        const auto bytes=codec::edge(record);
        const auto position=std::streamoff(32+record.edge_id*40);
        if (edge_file_.tellp()!=position) edge_file_.seekp(position);
        edge_file_.write(bytes.data(),bytes.size()); if (flush) edge_file_.flush();
        if (!edge_file_) return fail("graph edge write failed");
        edge_file_records_=std::max(edge_file_records_.load(),record.edge_id+1); return true;
    } catch (const std::exception& error) { return fail(error.what()); }
}
bool GraphStorage::writeBatch(const std::vector<NodeRecord>& nodes,const std::vector<EdgeRecord>& edges) {
    std::lock_guard lock(mutex_);
    if (!is_open_ || failed_) return false;
    for (const auto& node:nodes) if (!writeNodeUnlocked(node,false)) return false;
    node_file_.flush();
    if (!node_file_) return fail("batch node flush failed");
    for (const auto& edge:edges) if (!writeEdgeUnlocked(edge,false)) return false;
    edge_file_.flush();
    return bool(edge_file_) || fail("batch edge flush failed");
}
size_t GraphStorage::memoryBytesEstimate() const {
    std::lock_guard lock(mutex_);
    const auto jsonBytes=[](const auto& self,const nlohmann::json& value)->size_t {
        size_t bytes=sizeof(value);
        if (value.is_string()) {
            const auto& str=value.get_ref<const std::string&>();
            bytes+=sizeof(str)+(str.capacity()>std::string{}.capacity() ? str.capacity()+1 : 0);
        } else if (value.is_array()) {
            const auto& array=value.get_ref<const nlohmann::json::array_t&>();
            bytes+=sizeof(array)+(array.capacity()-array.size())*sizeof(nlohmann::json);
            for (const auto& child:array) bytes+=self(self,child);
        } else if (value.is_object()) {
            bytes+=sizeof(nlohmann::json::object_t);
            for (auto it=value.begin();it!=value.end();++it) {
                bytes+=sizeof(std::string)+4*sizeof(void*)+
                    (it.key().capacity()>std::string{}.capacity() ? it.key().capacity()+1 : 0);
                bytes+=self(self,it.value());
            }
        }
        return bytes;
    };
    return sizeof(*this)+jsonBytes(jsonBytes,metadata_);
}
bool GraphStorage::rememberBatch(const std::vector<Identity>& identities,
                                const std::map<TypeId,std::string>& types) {
    std::lock_guard lock(mutex_);
    if (!is_open_ || failed_) return false;
    for (const auto& node:identities) {
        metadata_["identities"][std::to_string(node.id)]=node.external_id;
        if (node.type!=kInvalidTypeId)
            metadata_["node_types"][std::to_string(node.type)]=node.type_name;
    }
    for (const auto& [id,name]:types) metadata_["edge_types"][std::to_string(id)]=name;
    return persistMetadata();
}
bool GraphStorage::updateNodeFlags(DenseId id,uint32_t flags) {
    std::lock_guard lock(mutex_); auto node=readNode(id); if (!node) return false;
    node->flags=flags; return writeNode(*node);
}
bool GraphStorage::updateNodeDegree(DenseId id,uint64_t out,uint64_t in) {
    std::lock_guard lock(mutex_); auto node=readNode(id); if (!node) return false;
    node->out_degree=out; node->in_degree=in; return writeNode(*node);
}
bool GraphStorage::updateEdgeFlags(EdgeId id,uint32_t flags) {
    std::lock_guard lock(mutex_); auto edge=readEdge(id); if (!edge) return false;
    edge->flags=flags; return writeEdge(*edge);
}
void GraphStorage::scanAllNodes(const std::function<bool(const NodeRecord&)>& callback) const {
    std::lock_guard lock(mutex_);
    for (uint64_t id=0;id<node_file_records_;++id) { auto node=readNode(id); if (!node) throw std::runtime_error("graph node scan failed"); if (!callback(*node)) break; }
}
void GraphStorage::scanAllEdges(const std::function<bool(const EdgeRecord&)>& callback) const {
    std::lock_guard lock(mutex_);
    for (uint64_t id=0;id<edge_file_records_;++id) { auto edge=readEdge(id); if (!edge) throw std::runtime_error("graph edge scan failed"); if (!callback(*edge)) break; }
}
std::optional<GraphMeta> GraphStorage::readMeta() const {
    std::lock_guard lock(mutex_); if (!is_open_) return std::nullopt;
    return decodeMeta(metadata_.at("meta"));
}
bool GraphStorage::persistMetadata() {
    try {
        if (!detail::AtomicWriteFile(meta_path_,metadataBytes(metadata_,generation_))) return fail("graph metadata publication failed");
        return true;
    } catch (const std::exception& error) { return fail(error.what()); }
}
bool GraphStorage::writeMeta(const GraphMeta& meta) {
    std::lock_guard lock(mutex_); if (!is_open_ || failed_) return false;
    try { auto value=metaJson(meta); (void)decodeMeta(value); metadata_["meta"]=value; return persistMetadata(); }
    catch (const std::exception& error) { return fail(error.what()); }
}
bool GraphStorage::updateMetaState(GraphState state) {
    std::lock_guard lock(mutex_); auto meta=readMeta(); if (!meta) return false;
    meta->state=state; return writeMeta(*meta);
}
bool GraphStorage::isMetaClean() const { const auto meta=readMeta(); return meta && meta->state==GraphState::Clean; }
bool GraphStorage::rememberNode(DenseId id,const ExtId& ext,TypeId type,const std::string& name) {
    std::lock_guard lock(mutex_); if (!is_open_ || failed_) return false;
    const auto id_key=std::to_string(id),type_key=std::to_string(type);
    if (metadata_["identities"].contains(id_key) && metadata_["identities"][id_key]==ext &&
        (type==kInvalidTypeId || (metadata_["node_types"].contains(type_key) && metadata_["node_types"][type_key]==name))) return true;
    metadata_["identities"][std::to_string(id)]=ext;
    if (type!=kInvalidTypeId) metadata_["node_types"][std::to_string(type)]=name;
    return persistMetadata();
}
bool GraphStorage::rememberEdgeType(TypeId type,const std::string& name) {
    std::lock_guard lock(mutex_); if (!is_open_ || failed_ || type==kInvalidTypeId) return false;
    const auto key=std::to_string(type);
    if (metadata_["edge_types"].contains(key) && metadata_["edge_types"][key]==name) return true;
    metadata_["edge_types"][std::to_string(type)]=name; return persistMetadata();
}
std::map<DenseId,ExtId> GraphStorage::identities() const {
    std::lock_guard lock(mutex_); std::map<DenseId,ExtId> result;
    for (auto it=metadata_.at("identities").begin();it!=metadata_.at("identities").end();++it) result.emplace(number(it.key()),it.value().get<std::string>());
    return result;
}
std::map<TypeId,std::string> GraphStorage::nodeTypes() const {
    std::lock_guard lock(mutex_); std::map<TypeId,std::string> result;
    for (auto it=metadata_.at("node_types").begin();it!=metadata_.at("node_types").end();++it) result.emplace(number(it.key()),it.value().get<std::string>());
    return result;
}
std::map<TypeId,std::string> GraphStorage::edgeTypes() const {
    std::lock_guard lock(mutex_); std::map<TypeId,std::string> result;
    for (auto it=metadata_.at("edge_types").begin();it!=metadata_.at("edge_types").end();++it) result.emplace(number(it.key()),it.value().get<std::string>());
    return result;
}
void GraphStorage::sync() {
    std::lock_guard lock(mutex_); if (node_file_.is_open()) node_file_.flush(); if (edge_file_.is_open()) edge_file_.flush();
}
bool GraphStorage::durableSync() {
    std::lock_guard lock(mutex_); if (!is_open_ || failed_) return false;
    sync();
    if (!node_file_ || !edge_file_ || !syncFile(node_path_) || !syncFile(edge_path_) ||
        !syncFile(meta_path_) || !syncFile(dir_,true)) return fail("graph durable sync failed");
    return true;
}
void GraphStorage::point(CompactionPoint point) { if (hook_) hook_(point); }
void GraphStorage::setCompactionHookForTesting(std::function<void(CompactionPoint)> hook) {
    std::lock_guard lock(mutex_); hook_=std::move(hook);
}
void GraphStorage::stage(uint64_t generation,const std::vector<NodeRecord>& nodes,
                         const std::vector<EdgeRecord>& edges,const Json& metadata,bool hooks) {
    std::string bytes=codec::header(1,generation);
    for (const auto& node:nodes) bytes+=codec::node(node);
    if (!detail::AtomicWriteFile(pathFor(dir_,name_,generation,".nex"),bytes)) throw std::runtime_error("graph node staging failed");
    if (hooks) point(CompactionPoint::NodesStaged);
    bytes=codec::header(2,generation); for (const auto& edge:edges) bytes+=codec::edge(edge);
    if (!detail::AtomicWriteFile(pathFor(dir_,name_,generation,".nexr"),bytes)) throw std::runtime_error("graph edge staging failed");
    if (hooks) point(CompactionPoint::EdgesStaged);
    if (!detail::AtomicWriteFile(pathFor(dir_,name_,generation,".nexl"),metadataBytes(metadata,generation))) throw std::runtime_error("graph metadata staging failed");
    if (hooks) point(CompactionPoint::MetadataStaged);
}
bool GraphStorage::compact(std::unordered_map<DenseId,DenseId>& node_remap,
                           std::unordered_map<EdgeId,EdgeId>& edge_remap) {
    std::lock_guard lock(mutex_); if (!is_open_ || failed_) return false;
    try {
        if (!durableSync()) return false;
        if (!validate()) return false;
        std::vector<NodeRecord> nodes; std::vector<EdgeRecord> edges;
        std::unordered_map<DenseId,DenseId> nr; std::unordered_map<EdgeId,EdgeId> er;
        scanAllNodes([&](const NodeRecord& record) {
            if (record.isActive()) { auto node=record; nr[uint64_t(record.dense_id)]=nodes.size(); node.dense_id=nodes.size(); node.in_degree=node.out_degree=0; nodes.push_back(node); } return true;
        });
        scanAllEdges([&](const EdgeRecord& record) {
            if (record.isActive()) {
                auto edge=record; edge.src=nr.at(record.src); edge.dst=nr.at(record.dst);
                er[uint64_t(record.edge_id)]=edges.size(); edge.edge_id=edges.size(); edges.push_back(edge);
                ++nodes[edge.src].out_degree; ++nodes[edge.dst].in_degree;
                if ((edge.flags&FLAG_UNDIRECTED) && edge.src!=edge.dst) {
                    ++nodes[edge.dst].out_degree; ++nodes[edge.src].in_degree;
                }
            } return true;
        });
        auto metadata=metadata_; metadata["identities"]=Json::object();
        for (const auto& [old_id,new_id]:nr) {
            const auto key=std::to_string(old_id);
            if (metadata_["identities"].contains(key)) metadata["identities"][std::to_string(new_id)]=metadata_["identities"][key];
        }
        auto meta=decodeMeta(metadata.at("meta"));
        meta.node_count=meta.active_nodes=nodes.size(); meta.edge_count=meta.active_edges=edges.size();
        meta.last_node_id=nodes.empty()?0:nodes.size()-1; meta.last_edge_id=edges.empty()?0:edges.size()-1;
        metadata["meta"]=metaJson(meta);
        const auto next=nextGeneration(); stage(next,nodes,edges,metadata,true);
        point(CompactionPoint::BeforeManifest);
        if (!detail::AtomicWriteFile(manifestPath(),manifest(next))) return fail("graph manifest publication failed; reopen required");
        point(CompactionPoint::ManifestPublished);
        if (!activate(next)) return false;
        node_remap=std::move(nr); edge_remap=std::move(er);
        point(CompactionPoint::Activated); return true;
    } catch (const std::exception& error) { return fail(std::string("graph compaction failed; reopen required: ")+error.what()); }
}
bool GraphStorage::compactNodes(std::unordered_map<DenseId,DenseId>& remap) {
    std::lock_guard lock(mutex_); if (!is_open_ || failed_) return false; remap.clear();
    scanAllNodes([&](const NodeRecord& node) { if (node.isActive()) remap[uint64_t(node.dense_id)]=remap.size(); return true; }); return true;
}
bool GraphStorage::compactEdges(const std::unordered_map<DenseId,DenseId>& expected,
                                std::unordered_map<EdgeId,EdgeId>& edges) {
    std::lock_guard lock(mutex_); std::unordered_map<DenseId,DenseId> current;
    if (!compactNodes(current) || current!=expected) return false;
    return compact(current,edges);
}
} // namespace nexora::graph
