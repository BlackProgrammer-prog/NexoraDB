cmake_minimum_required(VERSION 3.22)
project(NexoraDB)

set(CMAKE_CXX_STANDARD 20)

add_executable(NexoraDB main.cpp)

find_package(fmt CONFIG REQUIRED)
find_package(RocksDB CONFIG REQUIRED)

add_subdirectory(core)
add_subdirectory(query)
add_subdirectory(utils)

target_link_libraries(NexoraDB PRIVATE
        core_lib
        query_lib
        utils_lib
        fmt::fmt
        RocksDB::rocksdb
)

