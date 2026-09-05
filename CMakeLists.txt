cmake_minimum_required(VERSION 3.22)
project(NexoraDB VERSION 0.2.0 LANGUAGES CXX)

# Generate compile_commands.json where the selected generator supports it.
if (CMAKE_GENERATOR MATCHES "Ninja")
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
elseif (CMAKE_GENERATOR MATCHES "Unix Makefiles")
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
else ()
    set(CMAKE_EXPORT_COMPILE_COMMANDS OFF)
endif ()
# ─────────────────────────────────────────────────────────────
# §1  C++ Standard
# ─────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─────────────────────────────────────────────────────────────
# §2  Build Options
# ─────────────────────────────────────────────────────────────
option(NEXORA_BUILD_GRAPH   "Build GraphEngine"          ON)
option(NEXORA_BUILD_PYTHON  "Build Python bindings"      ON)
option(NEXORA_DEBUG_LOG     "Enable debug logging"       OFF)
option(NEXORA_ENABLE_ASAN   "Enable AddressSanitizer"    OFF)
option(NEXORA_ENABLE_UBSAN  "Enable UndefinedSanitizer"  OFF)
option(NEXORA_ENABLE_TSAN   "Enable ThreadSanitizer"     OFF)
option(NEXORA_ENABLE_LTO    "Enable LTO in optimized builds" OFF)
option(NEXORA_BUILD_BENCHMARKS "Build native C++ benchmarks" OFF)

# CMake supplies the optimization/debug flags for each configuration. Choose a
# predictable default only for single-config generators such as Make and Ninja.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
            Debug Release RelWithDebInfo MinSizeRel)
endif()

# Static project libraries are linked into the pybind11 shared module.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ─────────────────────────────────────────────────────────────
# §3  vcpkg toolchain
# ─────────────────────────────────────────────────────────────
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
            "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "vcpkg toolchain")
    message(STATUS "vcpkg: $ENV{VCPKG_ROOT}")
endif()

# ─────────────────────────────────────────────────────────────
# §4  Compiler flags
# ─────────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/W4 /WX- /MP /utf-8 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# ASan and TSan cannot be enabled together.
if(NEXORA_ENABLE_ASAN AND NEXORA_ENABLE_TSAN)
    message(FATAL_ERROR "ASan and TSan cannot be enabled together")
endif()

if(MSVC)
    if(NEXORA_ENABLE_ASAN)
        add_compile_options(/fsanitize=address)
        add_link_options(/fsanitize=address)
    endif()
    if(NEXORA_ENABLE_UBSAN OR NEXORA_ENABLE_TSAN)
        message(FATAL_ERROR
                "UBSan and TSan are currently supported only with GCC or Clang")
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    if(NEXORA_ENABLE_ASAN)
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
    if(NEXORA_ENABLE_UBSAN)
        add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=undefined)
    endif()
    if(NEXORA_ENABLE_TSAN)
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
    endif()
elseif(NEXORA_ENABLE_ASAN OR NEXORA_ENABLE_UBSAN OR NEXORA_ENABLE_TSAN)
    message(FATAL_ERROR "The selected compiler does not support the requested sanitizer")
endif()

# Let CMake select the compiler-specific IPO/LTO flags. This deliberately does
# not add -march=native, so Release binaries remain portable.
if(NEXORA_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(
            RESULT NEXORA_IPO_SUPPORTED
            OUTPUT NEXORA_IPO_ERROR
            LANGUAGES CXX)
    if(NEXORA_IPO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)

        if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            add_compile_options(
                    "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-flto=auto>"
            )

            add_link_options(
                    "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-flto=auto>"
            )
            
        endif ()
    else()
        message(FATAL_ERROR "LTO is not supported: ${NEXORA_IPO_ERROR}")
    endif()
endif()

if(NEXORA_DEBUG_LOG)
    add_compile_definitions(NEXORA_DEBUG NEXORA_GRAPH_DEBUG)
endif()

# ─────────────────────────────────────────────────────────────
# §5  External dependencies
# ─────────────────────────────────────────────────────────────
find_package(RocksDB CONFIG REQUIRED)
find_package(fmt     CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(Snappy  CONFIG QUIET)
find_package(zstd    CONFIG QUIET)

message(STATUS "Compiler : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "CMake    : ${CMAKE_VERSION}")
message(STATUS "System   : ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "RocksDB  : ${RocksDB_VERSION}")
message(STATUS "fmt      : ${fmt_VERSION}")
message(STATUS "Threads  : found")

# ─────────────────────────────────────────────────────────────
# §6  Internal library chain
#
#   nexora_query  (no deps)
#        ↑
#   nexora_core   ← nexora_query + RocksDB
#        ↑
#   nexora_graph  ← nexora_core + Threads
#        ↑
#   nexoradb (Python .so) ← nexora_graph
#   NexoraDB (test exe)   ← nexora_graph
# ─────────────────────────────────────────────────────────────

add_subdirectory(query)   # 1st — no deps
add_subdirectory(core)    # 2nd — needs nexora_query
#        core/CMakeLists.txt links nexora_query + RocksDB

if(NEXORA_BUILD_GRAPH)
    add_subdirectory(graph)
    # graph/CMakeLists.txt defines nexora_graph target
    # اینجا وابستگی cross-module را کامل می‌کنیم:
    target_link_libraries(nexora_graph PUBLIC nexora_core Threads::Threads)
    message(STATUS "GraphEngine : ENABLED")
else()
    message(STATUS "GraphEngine : disabled")
endif()

# ─────────────────────────────────────────────────────────────
# §7  Python binding module  (nexoradb.so)
# ─────────────────────────────────────────────────────────────
if(NEXORA_BUILD_PYTHON)
    # Use pybind11's new FindPython mode so Debug C++ builds still target the
    # ABI of the selected Python interpreter, not a Python debug ABI.
    set(PYBIND11_FINDPYTHON ON)
    find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
    unset(PYTHON_IS_DEBUG CACHE)
    find_package(pybind11 CONFIG REQUIRED)
    message(STATUS "Python   : ${Python_VERSION}")
    message(STATUS "pybind11 : ${pybind11_VERSION}")

    pybind11_add_module(nexoradb_py
            bindings/nexoradb_bind.cpp
    )

    # Embed reproducible build metadata in the extension. Python benchmarks
    # read it directly through nexoradb.build_info().
    target_compile_definitions(nexoradb_py PRIVATE
            NEXORA_PROJECT_VERSION="${PROJECT_VERSION}"
            NEXORA_BUILD_TYPE="$<CONFIG>"
            NEXORA_CMAKE_VERSION="${CMAKE_VERSION}"
            NEXORA_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
            NEXORA_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
            NEXORA_SYSTEM_NAME="${CMAKE_SYSTEM_NAME}"
            NEXORA_SYSTEM_PROCESSOR="${CMAKE_SYSTEM_PROCESSOR}"
            NEXORA_ROCKSDB_VERSION="${RocksDB_VERSION}"
            NEXORA_FMT_VERSION="${fmt_VERSION}"
            NEXORA_PYBIND11_VERSION="${pybind11_VERSION}"
            NEXORA_PYTHON_VERSION="${Python_VERSION}"
            NEXORA_LTO_ENABLED=$<AND:$<BOOL:${NEXORA_ENABLE_LTO}>,$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>>
            NEXORA_ASAN_ENABLED=$<BOOL:${NEXORA_ENABLE_ASAN}>
            NEXORA_UBSAN_ENABLED=$<BOOL:${NEXORA_ENABLE_UBSAN}>
            NEXORA_TSAN_ENABLED=$<BOOL:${NEXORA_ENABLE_TSAN}>
    )

    # نام فایل خروجی: nexoradb.cpython-3xx.so (بدون پیشوند lib)
    set_target_properties(nexoradb_py PROPERTIES
            OUTPUT_NAME "nexoradb"
            PREFIX      ""
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_custom_command(TARGET nexoradb_py POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:nexoradb_py>
                    "${CMAKE_BINARY_DIR}/$<TARGET_FILE_NAME:nexoradb_py>"
            COMMENT "Copying nexoradb Python module next to build-tree tests"
    )

    if(NOT MSVC AND NOT PYTHON_IS_DEBUG)
        target_compile_options(nexoradb_py PRIVATE -UPy_DEBUG)
    endif()

    target_include_directories(nexoradb_py PRIVATE
            ${CMAKE_SOURCE_DIR}   # برای "core/DocEngine.h", "graph/GraphManager.h"
    )

    # وابستگی به کتابخانه‌های C++
    if(NEXORA_BUILD_GRAPH)
        target_link_libraries(nexoradb_py PRIVATE nexora_graph_algorithms)
        target_compile_definitions(nexoradb_py PRIVATE NEXORA_BUILD_GRAPH)
    else()
        target_link_libraries(nexoradb_py PRIVATE nexora_core)
    endif()

    # محل نصب: کنار فایل‌های Python
    install(TARGETS nexoradb_py
            LIBRARY DESTINATION ${CMAKE_SOURCE_DIR}
    )

    message(STATUS "Python binding : ENABLED → nexoradb.so")
else()
    message(STATUS "Python binding : disabled")
endif()

# ─────────────────────────────────────────────────────────────
# §8  Test executable  (NexoraDB)
# ─────────────────────────────────────────────────────────────
add_executable(NexoraDB main.cpp)

target_include_directories(NexoraDB PRIVATE ${CMAKE_SOURCE_DIR})

if(NEXORA_BUILD_GRAPH)
    target_link_libraries(NexoraDB PRIVATE nexora_graph fmt::fmt)
    target_compile_definitions(NexoraDB PRIVATE NEXORA_BUILD_GRAPH)
else()
    target_link_libraries(NexoraDB PRIVATE nexora_core fmt::fmt)
endif()

if(NEXORA_BUILD_PYTHON)
    add_dependencies(NexoraDB nexoradb_py)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests/cpp)
endif ()

if (NEXORA_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks/cpp)
endif ()

# ─────────────────────────────────────────────────────────────
# §9  Build summary
# ─────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "══════════════════════════════════════════════")
message(STATUS "  NexoraDB Build Configuration")
message(STATUS "══════════════════════════════════════════════")
message(STATUS "  C++ Standard   : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type     : ${CMAKE_BUILD_TYPE}")
message(STATUS "  GraphEngine    : ${NEXORA_BUILD_GRAPH}")
message(STATUS "  Python Binding : ${NEXORA_BUILD_PYTHON}")
message(STATUS "  Debug Log      : ${NEXORA_DEBUG_LOG}")
message(STATUS "  ASAN           : ${NEXORA_ENABLE_ASAN}")
message(STATUS "  UBSAN          : ${NEXORA_ENABLE_UBSAN}")
message(STATUS "  TSAN           : ${NEXORA_ENABLE_TSAN}")
message(STATUS "  LTO            : ${NEXORA_ENABLE_LTO}")
message(STATUS "  C++ Benchmarks : ${NEXORA_BUILD_BENCHMARKS}")
message(STATUS "")
message(STATUS "  nexora_query → nexora_core → nexora_graph")
message(STATUS "                                    ↓")
message(STATUS "                      nexoradb.so (Python)")
message(STATUS "                      NexoraDB    (C++ test)")
message(STATUS "══════════════════════════════════════════════")
message(STATUS "")
