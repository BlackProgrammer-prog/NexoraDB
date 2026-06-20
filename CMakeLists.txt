#cmake_minimum_required(VERSION 3.22)
#project(NexoraDB)
#
#set(CMAKE_CXX_STANDARD 20)
#
#add_executable(NexoraDB main.cpp)
#
#find_package(fmt CONFIG REQUIRED)
#find_package(RocksDB CONFIG REQUIRED)
#find_package(Snappy CONFIG QUIET)
#find_package(zstd   CONFIG QUIET)
#
#
#add_subdirectory(core)
#add_subdirectory(query)
#add_subdirectory(utils)
#
#target_link_libraries(nexora_core PUBLIC nexora_query)
#
#target_link_libraries(NexoraDB PRIVATE
#        nexora_core
#        utils_lib
#        fmt::fmt
#        RocksDB::rocksdb
#)
#

cmake_minimum_required(VERSION 3.22)
project(NexoraDB CXX)

# ─────────────────────────────────────────────────────────────
# §1  C++ Standard
# ─────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─────────────────────────────────────────────────────────────
# §2  Build Options
# ─────────────────────────────────────────────────────────────
option(NEXORA_BUILD_GRAPH "Build GraphEngine (LiveGraph, GraphManager, ...)" ON)
option(NEXORA_DEBUG_LOG   "Enable debug logging (NX_LOG macros)"             OFF)
option(NEXORA_ENABLE_ASAN "Enable AddressSanitizer"                          OFF)

# ─────────────────────────────────────────────────────────────
# §3  vcpkg toolchain (اگر VCPKG_ROOT تنظیم شده باشد)
# ─────────────────────────────────────────────────────────────
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
            "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "vcpkg toolchain")
    message(STATUS "vcpkg toolchain: $ENV{VCPKG_ROOT}")
endif()

# ─────────────────────────────────────────────────────────────
# §4  Compiler flags
# ─────────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/W4 /WX- /O2 /MP /utf-8)
else()
    add_compile_options(
            -Wall -Wextra -Wpedantic
            -Wno-unused-parameter
            -O2 -fPIC
    )
    if(NEXORA_ENABLE_ASAN)
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
endif()

if(NEXORA_DEBUG_LOG)
    add_compile_definitions(NEXORA_DEBUG NEXORA_GRAPH_DEBUG)
endif()

# ─────────────────────────────────────────────────────────────
# §5  وابستگی‌های خارجی (vcpkg)
# ─────────────────────────────────────────────────────────────
find_package(RocksDB CONFIG REQUIRED)
message(STATUS "RocksDB: found")

find_package(fmt CONFIG REQUIRED)
message(STATUS "fmt: found")

# Threads برای GraphEngine (shared_mutex, thread)
find_package(Threads REQUIRED)
message(STATUS "Threads: found")

# فشرده‌سازی اضافی (اختیاری)
find_package(Snappy CONFIG QUIET)
find_package(zstd   CONFIG QUIET)

# ─────────────────────────────────────────────────────────────
# §6  زیرماژول‌ها — ترتیب مهم است!
#
#  زنجیره وابستگی:
#    nexora_query   (بدون وابستگی داخلی)
#        ↑
#    nexora_core    (← nexora_query + RocksDB)
#        ↑
#    nexora_graph   (← nexora_core + Threads)
#        ↑
#    NexoraDB exe   (← nexora_graph یا nexora_core)
# ─────────────────────────────────────────────────────────────

# 1. Query Layer
add_subdirectory(query)

# 2. Core / DocEngine
add_subdirectory(core)

# 3. Graph Engine
if(NEXORA_BUILD_GRAPH)
    add_subdirectory(graph)
    message(STATUS "GraphEngine: ENABLED")
else()
    message(STATUS "GraphEngine: disabled")
endif()

# ─────────────────────────────────────────────────────────────
# §7  وابستگی‌های cross-module (بعد از تعریف همه targets)
# ─────────────────────────────────────────────────────────────

# core → query  (قبلاً در core/CMakeLists.txt تعریف شده ولی اینجا هم تکرار می‌کنیم)
# target_link_libraries(nexora_core PUBLIC nexora_query)   ← already in core/CMakeLists.txt

# graph → core + Threads
if(NEXORA_BUILD_GRAPH)
    target_link_libraries(nexora_graph PUBLIC nexora_core Threads::Threads)
endif()

# ─────────────────────────────────────────────────────────────
# §8  Executable — NexoraDB (تست)
# ─────────────────────────────────────────────────────────────
add_executable(NexoraDB main.cpp)

if(NEXORA_BUILD_GRAPH)
    target_link_libraries(NexoraDB PRIVATE
            nexora_graph    # graph → core → query → RocksDB (همه را می‌آورد)
            fmt::fmt
    )
else()
    target_link_libraries(NexoraDB PRIVATE
            nexora_core     # core → query → RocksDB
            fmt::fmt
    )
endif()

target_include_directories(NexoraDB PRIVATE
        ${CMAKE_SOURCE_DIR}   # برای include هایی مثل "core/DocEngine.h"
)

# تعریف ماکرو NEXORA_BUILD_GRAPH برای main.cpp تا #if defined() کار کند
if(NEXORA_BUILD_GRAPH)
    target_compile_definitions(NexoraDB PRIVATE NEXORA_BUILD_GRAPH)
endif()

# ─────────────────────────────────────────────────────────────
# §9  خلاصه Build
# ─────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "══════════════════════════════════════════════")
message(STATUS "  NexoraDB Build Configuration")
message(STATUS "══════════════════════════════════════════════")
message(STATUS "  C++ Standard  : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  GraphEngine   : ${NEXORA_BUILD_GRAPH}")
message(STATUS "  Debug Log     : ${NEXORA_DEBUG_LOG}")
message(STATUS "  ASAN          : ${NEXORA_ENABLE_ASAN}")
message(STATUS "")
message(STATUS "  Target chain:")
message(STATUS "    nexora_query  (Condition, UpdateSpec, Evaluator)")
message(STATUS "    nexora_core   (DocEngine) → nexora_query + RocksDB")
if(NEXORA_BUILD_GRAPH)
    message(STATUS "    nexora_graph  (LiveGraph, GraphManager) → nexora_core + Threads")
endif()
message(STATUS "    NexoraDB exe  → nexora_graph (or nexora_core)")
message(STATUS "══════════════════════════════════════════════")
message(STATUS "")