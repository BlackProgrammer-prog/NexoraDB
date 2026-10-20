#pragma once
#include <cstddef>

namespace nexora::graph {
struct GraphMemoryUsage {
    size_t object_bytes=0,node_records_bytes=0,adjacency_headers_bytes=0;
    size_t adjacency_payload_bytes=0,edge_store_bytes=0;
    size_t reverse_identity_bytes=0,forward_identity_bytes_estimate=0;
    size_t type_registry_bytes_estimate=0,free_ids_bytes_estimate=0;
    size_t wal_bytes_estimate=0,storage_bytes_estimate=0;
    size_t total_bytes_estimate=0;
    size_t budget_bytes=0,admission_charged_bytes=0;
    size_t edge_slot_capacity=0,sparse_edges=0,inline_adjacency_lists=0,heavy_adjacency_lists=0;
};
}
