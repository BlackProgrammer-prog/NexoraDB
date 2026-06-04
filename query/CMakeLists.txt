add_library(query_lib STATIC QueryMatcher.cpp)

target_include_directories(query_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(query_lib PUBLIC core_lib)
