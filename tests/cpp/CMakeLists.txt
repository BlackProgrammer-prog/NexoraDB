find_package(GTest CONFIG REQUIRED)

include(GoogleTest)

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
    gtest_discover_tests(
            nexora_graph_tests
            TEST_PREFIX "cpp."
            DISCOVERY_TIMEOUT 30
    )

endif ()