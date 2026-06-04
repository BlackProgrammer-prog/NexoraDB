add_library(core_lib STATIC DocEngine.cpp)


target_include_directories(core_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})


target_link_libraries(core_lib PUBLIC RocksDB::rocksdb)
