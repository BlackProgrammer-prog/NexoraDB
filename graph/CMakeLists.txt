cmake_minimum_required(VERSION 3.20)

# ─────────────────────────────────────────────────────────────
# کتابخانه: nexora_graph
#
# ساختار فایل‌های graph/:
#
#   ── Types ──
#   GraphTypes.h          ← تمام انواع مشترک: DenseId, Direction,
#                           NodeMappingDef, EdgeMappingDef, GraphDefinition,
#                           NodeRecord, EdgeRecord, WalRecord, AdjEntry,
#                           ChunkedSortedVector, NodeAdj, GraphStats, ...
#
#   ── Persistence لایه دیسک ──
#   GraphStorage.h/cpp    ← فایل‌های .nex / .nexr / .nexl (Fixed-size records)
#   GraphWAL.h/cpp        ← Write-Ahead Log (graph.wal) با retention policy
#   GraphIdStore.h/cpp    ← DenseId ↔ ExtId در RocksDB (crash-safe)
#
#   ── Runtime لایه RAM ──
#   LiveGraph.h/cpp       ← گراف پویا: AdjacencyList + DenseId + FreeStack
#   StaticGraph.h/cpp     ← snapshot read-only برای JobAlgorithm
#
#   ── Orchestrator ──
#   GraphManager.h/cpp    ← اتصال DocEngine ↔ LiveGraph
#                           Build/Render/Refresh/onDocumentXxx
#
#   ── Algorithms (تیم الگوریتم) ──
#   algorithms/
#   ├── AlgorithmBase.h/cpp ← LockAlgorithm + JobAlgorithm + StaticGraphView
#   ├── CommonFollowers.cpp ← نمونه LockAlgorithm
#   └── PageRank.cpp        ← نمونه JobAlgorithm
#
# ─────────────────────────────────────────────────────────────

# ══════════════════════════════════════════════════════════════
# §1  nexora_graph — کتابخانه اصلی
# ══════════════════════════════════════════════════════════════

add_library(nexora_graph STATIC
        # Types (header-only — نیازی به .cpp ندارد)
        Graphtypes.h

        # Disk Layer
        Graphstorage.h
        Graphstorage.cpp
        Graphwal.h
        Graphwal.cpp
        GraphIdStore.h
        GraphIdStore.cpp

        # RAM Layer
        Livegraph.h
        Livegraph.cpp
        StaticGraph.h
        StaticGraph.cpp

        # Orchestrator
        GraphManager.h
        GraphManager.cpp
)

target_include_directories(nexora_graph
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include/nexoradb/graph>
)

target_link_libraries(nexora_graph
        PUBLIC
        nexora_core          # DocEngine + RocksDB + QueryLayer
        Threads::Threads     # std::shared_mutex, std::thread
)

target_compile_features(nexora_graph PUBLIC cxx_std_20)

if(NEXORA_DEBUG_LOG)
    target_compile_definitions(nexora_graph PUBLIC NEXORA_GRAPH_DEBUG)
endif()

# ══════════════════════════════════════════════════════════════
# §2  nexora_graph_algorithms — تیم الگوریتم
#     این target جداست تا تیم الگوریتم مستقل کامپایل کند
# ══════════════════════════════════════════════════════════════

set(ALGO_BASE_SOURCES
        algorithms/AlgorithmBase.h
        algorithms/AlgorithmBase.cpp
        algorithms/BuiltinAlgorithms.h
)

# تیم الگوریتم فایل‌های خود را اینجا اضافه می‌کند:
set(ALGO_USER_SOURCES
        algorithms/MutualFriends.cpp
        algorithms/ConnectedComponents.cpp
        algorithms/MostConnected.cpp
        algorithms/NetworkStats.cpp
        algorithms/CommunityDetection.cpp
        algorithms/AllDistances.cpp
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/algorithms/AlgorithmBase.cpp")
    add_library(nexora_graph_algorithms STATIC
            ${ALGO_BASE_SOURCES}
            ${ALGO_USER_SOURCES}
    )

    target_include_directories(nexora_graph_algorithms
            PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/algorithms>
            $<INSTALL_INTERFACE:include/nexoradb/graph/algorithms>
    )

    target_link_libraries(nexora_graph_algorithms
            PUBLIC
            nexora_graph
    )

    target_compile_features(nexora_graph_algorithms PUBLIC cxx_std_20)
    message(STATUS "[graph] nexora_graph_algorithms: configured")
else()
    message(STATUS "[graph] algorithms/ not yet populated — skipping")
endif()

# ══════════════════════════════════════════════════════════════
# §3  نصب
# ══════════════════════════════════════════════════════════════

install(TARGETS nexora_graph
        EXPORT  NexoraTargets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
)

install(FILES
        Graphtypes.h
        Graphstorage.h
        Graphwal.h
        GraphIdStore.h
        Livegraph.h
        StaticGraph.h
        GraphManager.h
        DESTINATION include/nexoradb/graph
)

install(DIRECTORY algorithms/
        DESTINATION include/nexoradb/graph/algorithms
        FILES_MATCHING PATTERN "*.h"
)

message(STATUS "[graph] nexora_graph configured")
message(STATUS "[graph]   Files: GraphTypes + GraphStorage + GraphWAL + GraphIdStore")
message(STATUS "[graph]          + LiveGraph + StaticGraph + GraphManager")
message(STATUS "[graph]   Deps:  nexora_core + Threads")