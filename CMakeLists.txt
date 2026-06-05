cmake_minimum_required(VERSION 3.22)
project(NexoraDB)

set(CMAKE_CXX_STANDARD 20)

add_executable(NexoraDB main.cpp)

find_package(fmt CONFIG REQUIRED)
find_package(RocksDB CONFIG REQUIRED)
find_package(Snappy CONFIG QUIET)
find_package(zstd   CONFIG QUIET)


add_subdirectory(core)
add_subdirectory(query)
add_subdirectory(utils)

target_link_libraries(nexora_core PUBLIC nexora_query)

target_link_libraries(NexoraDB PRIVATE
        utils_lib
        fmt::fmt
        RocksDB::rocksdb
)

