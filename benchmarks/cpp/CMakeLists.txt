find_package(benchmark CONFIG REQUIRED)

add_executable(nexora_benchmark
        nexora_benchmark.cpp
)

target_include_directories(nexora_benchmark
        PRIVATE
        ${PROJECT_SOURCE_DIR}
)

target_link_libraries(nexora_benchmark
        PRIVATE
        benchmark::benchmark
        nexora_core
        Threads::Threads
)

if (NEXORA_BUILD_GRAPH)
    target_link_libraries(nexora_benchmark
            PRIVATE
            nexora_graph_algorithms
    )

    target_compile_definitions(nexora_benchmark
            PRIVATE
            NEXORA_BUILD_GRAPH
    )

endif ()


target_compile_definitions(nexora_benchmark PRIVATE
        NEXORA_BENCH_PROJECT_VERSION="${PROJECT_VERSION}"
        NEXORA_BENCH_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
        NEXORA_BENCH_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
        NEXORA_BENCH_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
        NEXORA_BENCH_ROCKSDB_VERSION="${RocksDB_VERSION}"
        NEXORA_BENCH_SYSTEM="${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}"
)

target_compile_features(nexora_benchmark PRIVATE cxx_std_20)

