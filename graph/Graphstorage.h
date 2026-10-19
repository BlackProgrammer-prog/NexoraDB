#pragma once
#include "Graphtypes.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nexora::graph {
enum class CompactionPoint { NodesStaged, EdgesStaged, MetadataStaged,
                             BeforeManifest, ManifestPublished, Activated };
class GraphStorage {
public:
    struct Identity { DenseId id; ExtId external_id; TypeId type; std::string type_name; };
    bool rememberBatch(const std::vector<Identity>&,
                       const std::map<TypeId,std::string>& edge_types);
    bool writeBatch(const std::vector<NodeRecord>&,const std::vector<EdgeRecord>&);
    GraphStorage(const std::filesystem::path& dir,const std::string& name);
    ~GraphStorage();
    GraphStorage(const GraphStorage&)=delete;
    GraphStorage& operator=(const GraphStorage&)=delete;
    bool open();
    void close();
    bool isOpen() const noexcept { return is_open_; }
    std::string lastError() const;
    uint64_t generation() const noexcept { return generation_; }
    std::optional<GraphMeta> readMeta() const;
    bool writeMeta(const GraphMeta&);
    bool updateMetaState(GraphState);
    bool isMetaClean() const;
    bool writeNode(const NodeRecord&);
    std::optional<NodeRecord> readNode(DenseId) const;
    bool updateNodeFlags(DenseId,uint32_t);
    bool updateNodeDegree(DenseId,uint64_t,uint64_t);
    uint64_t nodeFileSize() const noexcept { return node_file_records_; }
    bool writeEdge(const EdgeRecord&);
    std::optional<EdgeRecord> readEdge(EdgeId) const;
    bool updateEdgeFlags(EdgeId,uint32_t);
    uint64_t edgeFileSize() const noexcept { return edge_file_records_; }
    void scanAllNodes(const std::function<bool(const NodeRecord&)>&) const;
    void scanAllEdges(const std::function<bool(const EdgeRecord&)>&) const;
    bool rememberNode(DenseId,const ExtId&,TypeId,const std::string& type_name);
    bool rememberEdgeType(TypeId,const std::string&);
    std::map<DenseId,ExtId> identities() const;
    std::map<TypeId,std::string> nodeTypes() const;
    std::map<TypeId,std::string> edgeTypes() const;
    bool compact(std::unordered_map<DenseId,DenseId>&,
                 std::unordered_map<EdgeId,EdgeId>&);
    // Compatibility pair: first plans only, second atomically publishes both.
    bool compactNodes(std::unordered_map<DenseId,DenseId>&);
    bool compactEdges(const std::unordered_map<DenseId,DenseId>&,
                      std::unordered_map<EdgeId,EdgeId>&);
    std::filesystem::path nodePath() const { return node_path_; }
    std::filesystem::path edgePath() const { return edge_path_; }
    std::filesystem::path metaPath() const { return meta_path_; }
    std::filesystem::path manifestPath() const { return dir_/(name_+".manifest"); }
    void sync();
    bool durableSync();
    void setCompactionHookForTesting(std::function<void(CompactionPoint)> hook);
private:
    std::filesystem::path dir_,node_path_,edge_path_,meta_path_;
    std::string name_;
    mutable std::recursive_mutex mutex_;
    mutable std::fstream node_file_,edge_file_;
    std::atomic<bool> is_open_{false};
    std::atomic<uint64_t> node_file_records_{0},edge_file_records_{0},generation_{0};
    nlohmann::json metadata_;
    mutable std::string error_;
    mutable bool failed_=false;
    int lock_fd_=-1;
    std::function<void(CompactionPoint)> hook_;
    bool fail(const std::string&);
    void selectPaths(uint64_t);
    bool activate(uint64_t);
    bool persistMetadata();
    bool writeNodeUnlocked(const NodeRecord&,bool flush);
    bool writeEdgeUnlocked(const EdgeRecord&,bool flush);
    bool validate();
    uint64_t nextGeneration() const;
    void stage(uint64_t,const std::vector<NodeRecord>&,const std::vector<EdgeRecord>&,
               const nlohmann::json&,bool hooks);
    void point(CompactionPoint);
};
} // namespace nexora::graph
