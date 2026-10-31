#pragma once
#include "Livegraph.h"

namespace nexora::graph {
struct BulkOptions {
    size_t max_records=4096;
    size_t max_buffer_bytes=8*1024*1024;
};
struct BulkStats {
    size_t batches=0, nodes_created=0, edges_created=0, edges_deduplicated=0;
    size_t peak_buffer_bytes=0;
};
// Synchronous producer backpressure. No implicit commit from the destructor.
// A batch is redoable, not an all-or-nothing transaction.
class BulkGraphBuilder {
    std::shared_ptr<nexora::core::BudgetLease> buffer_credit_;
public:
    explicit BulkGraphBuilder(LiveGraph&,BulkOptions={});
    BulkGraphBuilder(const BulkGraphBuilder&)=delete;
    BulkGraphBuilder& operator=(const BulkGraphBuilder&)=delete;
    void addNode(const ExtId&,const std::string& type);
    void addEdge(const ExtId& src,const ExtId& dst,const std::string& type,bool directed=true);
    void flush();
    const BulkStats& stats() const noexcept { return stats_; }
    size_t bufferedBytes() const noexcept { return buffered_bytes_; }
    enum class Point { WalDurable, DataWritten, Checkpointed };
    void setHookForTesting(std::function<void(Point)> hook) { hook_=std::move(hook); }
private:
    struct Node { ExtId id; std::string type; };
    struct Edge { ExtId src,dst; std::string type; bool directed; };
    LiveGraph& graph_;
    BulkOptions options_;
    BulkStats stats_;
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    size_t buffered_bytes_=0;
    bool failed_=false;
    std::function<void(Point)> hook_;
    void admit(size_t string_bytes);
};
}
