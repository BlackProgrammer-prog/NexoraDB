#add_library(core_lib STATIC DocEngine.cpp)
#
#
#target_include_directories(core_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
#
#
#target_link_libraries(core_lib PUBLIC RocksDB::rocksdb)

cmake_minimum_required(VERSION 3.22)

# ─────────────────────────────────────────────────────────────
# کتابخانه: nexora_core
#
# وابستگی‌ها:
#   - nexora_query  (Condition, UpdateSpec, Evaluator)
#   - RocksDB       (از طریق vcpkg)
#
# مصرف‌کنندگان:
#   - nexora_graph  (GraphEngine — لایه بالاتر، از DocEngine استفاده می‌کند)
#   - bindings/python (Cython)
#   - tests
#
# نکته مهم:
#   nexora_query باید پیش از nexora_core در CMake اضافه شود.
#   ترتیب add_subdirectory در root CMakeLists.txt:
#     1. add_subdirectory(query)    ← اول
#     2. add_subdirectory(core)     ← دوم
#     3. target_link_libraries(nexora_core PUBLIC nexora_query)
# ─────────────────────────────────────────────────────────────

add_library(nexora_core STATIC
        DocEngine.cpp
        DocEngine.h
        IndexCodec.cpp
        IndexCodec.h
)

# ─────────────────────────────────────────────────────────────
# Include Directories
# ─────────────────────────────────────────────────────────────
target_include_directories(nexora_core
        PUBLIC
        # دیگر ماژول‌ها با target_link_libraries(my_module PRIVATE nexora_core)
        # می‌توانند از هدرهای core استفاده کنند.
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        # برای دسترسی به query/ با مسیر "query/Condition.h"
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include/nexoradb>
)

# ─────────────────────────────────────────────────────────────
# وابستگی‌ها
# ─────────────────────────────────────────────────────────────

# nexora_query را PUBLIC می‌کنیم تا:
#   - GraphEngine که به nexora_core لینک می‌کند، به Evaluator/Condition هم دسترسی داشته باشد
#   - Cython bindings بتواند مستقیم Condition و UpdateSpec بسازد
target_link_libraries(nexora_core
        PUBLIC
        nexora_query
        RocksDB::rocksdb
)

# فشرده‌سازی اضافی (در صورت وجود)
if(TARGET Snappy::snappy)
    target_link_libraries(nexora_core PUBLIC Snappy::snappy)
endif()

if(TARGET zstd::libzstd_shared)
    target_link_libraries(nexora_core PUBLIC zstd::libzstd_shared)
elseif(TARGET zstd::libzstd_static)
    target_link_libraries(nexora_core PUBLIC zstd::libzstd_static)
endif()

# ─────────────────────────────────────────────────────────────
# تنظیمات کامپایل
# ─────────────────────────────────────────────────────────────
target_compile_features(nexora_core PUBLIC cxx_std_20)

if(NEXORA_DEBUG_LOG)
    target_compile_definitions(nexora_core PUBLIC NEXORA_DEBUG)
endif()

# ─────────────────────────────────────────────────────────────
# نصب (برای packaging)
# ─────────────────────────────────────────────────────────────
install(TARGETS nexora_core
        EXPORT  NexoraTargets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
)

install(FILES DocEngine.h
        DESTINATION include/nexoradb/core
)

message(STATUS "[core] nexora_core configured")
message(STATUS "[core]   depends on: nexora_query + RocksDB")
