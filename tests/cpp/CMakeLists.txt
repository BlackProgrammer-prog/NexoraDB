find_package(GTest CONFIG REQUIRED)

include(GoogleTest)

# GCC's ThreadSanitizer can fail before main() on WSL2 with
# "unexpected memory mapping" while ASLR is enabled.  GoogleTest also runs
# test binaries during discovery, so use the emulator hook for both discovery
# and CTest execution.  Keep this workaround scoped to TSAN builds on WSL.
function(nexora_configure_tsan_wsl_launcher target_name)
    if (NOT NEXORA_ENABLE_TSAN OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        return()
    endif ()

    if (NOT EXISTS "/proc/version")
        return()
    endif ()

    file(READ "/proc/version" nexora_proc_version)
    string(TOLOWER "${nexora_proc_version}" nexora_proc_version_lower)
    if (NOT nexora_proc_version_lower MATCHES "microsoft")
        return()
    endif ()

    find_program(NEXORA_SETARCH_EXECUTABLE setarch REQUIRED)
    set_property(
            TARGET ${target_name}
            PROPERTY CROSSCOMPILING_EMULATOR
            "${NEXORA_SETARCH_EXECUTABLE};${CMAKE_SYSTEM_PROCESSOR};-R"
    )
endfunction()

add_executable(nexora_doc_tests
        test_doc_engine.cpp
        test_rocks_tuning.cpp
        test_maintenance.cpp
        test_operations.cpp
)
if (NEXORA_BUILD_PYTHON)
    pybind11_add_module(nexora_conversion_probe python_conversion_probe.cpp)
    if (NOT MSVC AND NOT PYTHON_IS_DEBUG)
        target_compile_options(nexora_conversion_probe PRIVATE -UPy_DEBUG)
    endif ()
endif ()

target_link_libraries(nexora_doc_tests
        PRIVATE
        nexora_core
        GTest::gtest_main
)

target_include_directories(nexora_doc_tests
        PRIVATE
        ${PROJECT_SOURCE_DIR}
)
add_executable(nexora_rocks_layout_probe rocks_layout_probe.cpp)
target_link_libraries(nexora_rocks_layout_probe PRIVATE nexora_core)
add_executable(nexora_rocks_workload_probe rocks_workload_probe.cpp)
target_link_libraries(nexora_rocks_workload_probe PRIVATE nexora_core Threads::Threads)

nexora_configure_tsan_wsl_launcher(nexora_doc_tests)

gtest_discover_tests(
        nexora_doc_tests
        TEST_PREFIX "cpp."
        DISCOVERY_TIMEOUT 30
)

if (NEXORA_BUILD_GRAPH)
    add_executable(nexora_graph_threading_probe graph_threading_probe.cpp)
    target_link_libraries(nexora_graph_threading_probe PRIVATE nexora_graph)
    add_executable(nexora_graph_phase5_ingestion_probe graph_phase5_ingestion_probe.cpp)
    target_link_libraries(nexora_graph_phase5_ingestion_probe PRIVATE nexora_graph)
    add_executable(nexora_graph_phase5_memory_probe graph_phase5_memory_probe.cpp)
    target_link_libraries(nexora_graph_phase5_memory_probe PRIVATE nexora_graph)
    add_executable(nexora_graph_memory_probe graph_memory_probe.cpp)
    add_executable(nexora_graph_snapshot_probe graph_snapshot_probe.cpp)
    target_link_libraries(nexora_graph_snapshot_probe PRIVATE nexora_graph)
    target_link_libraries(nexora_graph_memory_probe PRIVATE nexora_graph)
    add_executable(nexora_graph_tests
            test_graph.cpp
            test_graph_recovery.cpp
            test_graph_wal.cpp
            test_graph_storage.cpp
            test_graph_bulk.cpp
            test_graph_memory.cpp
            test_graph_snapshot.cpp
            test_graph_phase5.cpp
            test_concurrency.cpp
            test_threading_contract.cpp
            test_job_scheduler.cpp
    )
    target_link_libraries(nexora_graph_tests
            PRIVATE
            nexora_graph
            nexora_graph_algorithms
            GTest::gtest_main
            Threads::Threads
    )
    target_link_directories(nexora_graph_tests
            PRIVATE
            ${PROJECT_SOURCE_DIR}
    )
    nexora_configure_tsan_wsl_launcher(nexora_graph_tests)
    gtest_discover_tests(
            nexora_graph_tests
            TEST_PREFIX "cpp."
            DISCOVERY_TIMEOUT 30
            PROPERTIES TIMEOUT 60
    )

endif ()
