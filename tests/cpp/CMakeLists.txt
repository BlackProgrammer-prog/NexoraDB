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
)

target_link_libraries(nexora_doc_tests
        PRIVATE
        nexora_core
        GTest::gtest_main
)

target_include_directories(nexora_doc_tests
        PRIVATE
        ${PROJECT_SOURCE_DIR}
)

nexora_configure_tsan_wsl_launcher(nexora_doc_tests)

gtest_discover_tests(
        nexora_doc_tests
        TEST_PREFIX "cpp."
        DISCOVERY_TIMEOUT 30
)

if (NEXORA_BUILD_GRAPH)
    add_executable(nexora_graph_tests
            test_graph.cpp
            test_concurrency.cpp
    )
    target_link_libraries(nexora_graph_tests
            PRIVATE
            nexora_graph
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
    )

endif ()
