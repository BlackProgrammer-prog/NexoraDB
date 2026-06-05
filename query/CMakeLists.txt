#add_library(query_lib STATIC QueryMatcher.cpp)
#
#target_include_directories(query_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
#
#target_link_libraries(query_lib PUBLIC core_lib)


cmake_minimum_required(VERSION 3.22)

# ─────────────────────────────────────────────────────────────
# کتابخانه: nexora_query
#
# مسئولیت:
#   - تعریف Condition، UpdateSpec، QueryOptions (header-only بخشی)
#   - پیاده‌سازی Evaluator (Match + Apply + Project)
#   - پیاده‌سازی JsonAdapter (MVP) و interface IBsonAdapter
#
# مصرف‌کنندگان:
#   - nexora_core (DocEngine از Condition/UpdateSpec/Evaluator استفاده می‌کند)
#   - nexora_graph (GraphEngine از Evaluator::Match برای فیلتر استفاده می‌کند)
#   - bindings/python (Cython مستقیماً Condition/UpdateSpec می‌سازد)
# ─────────────────────────────────────────────────────────────

add_library(nexora_query STATIC
        Evaluator.cpp
        Evaluator.h
        Condition.h
        UpdateSpec.h
)

# ─────────────────────────────────────────────────────────────
# Include Directories
# ─────────────────────────────────────────────────────────────
target_include_directories(nexora_query
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include/nexoradb/query>
)

# ─────────────────────────────────────────────────────────────
# وابستگی‌ها
# ─────────────────────────────────────────────────────────────
# nexora_query عمداً به nexora_core وابسته نیست
# این اجازه می‌دهد مستقل تست شود.
# nexora_core به nexora_query وابسته است (نه برعکس).

target_compile_features(nexora_query PUBLIC cxx_std_20)

if(NEXORA_DEBUG_LOG)
    target_compile_definitions(nexora_query PUBLIC NEXORA_DEBUG)
endif()

# ─────────────────────────────────────────────────────────────
# نصب
# ─────────────────────────────────────────────────────────────
install(TARGETS nexora_query
        EXPORT  NexoraTargets
        ARCHIVE DESTINATION lib
)

install(FILES
        Condition.h
        UpdateSpec.h
        Evaluator.h
        DESTINATION include/nexoradb/query
)

message(STATUS "[query] nexora_query configured")