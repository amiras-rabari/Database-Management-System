# --- db_opt: the query optimizer (statistics, estimates, rewrites, DP) -------
set(DB_OPT_SOURCES
    stats/statistics.cpp
    stats/histogram.cpp
    stats/analyze.cpp
    selectivity/selectivity.cpp
    selectivity/cardinality.cpp
    rewrite/rewrite.cpp
    cost/cost.cpp
    physical/to_physical.cpp
    enumerate/dp.cpp
    planner/optimizer.cpp
    physical/explain.cpp
)
add_library(db_opt STATIC ${DB_OPT_SOURCES})
target_include_directories(db_opt PUBLIC include)
# opt may use common, storage and exec (root CLAUDE.md dependency rule), never
# sql: it receives the logical plan as a common PlanNode. db_exec brings
# db_storage and db_common with it; they are listed so the rule is visible.
target_link_libraries(db_opt PUBLIC db_exec db_storage db_common)

# --- Tests ------------------------------------------------------------------
add_executable(opt_tests
    tests/test_main.cpp
    tests/statistics_tests.cpp
    tests/histogram_tests.cpp
    tests/analyze_tests.cpp
    tests/selectivity_tests.cpp
    tests/cardinality_tests.cpp
    tests/rewrite_tests.cpp
    tests/cost_tests.cpp
    tests/physical_tests.cpp
    tests/dp_tests.cpp
    tests/planner_tests.cpp
    tests/explain_tests.cpp
)
target_link_libraries(opt_tests PRIVATE db_opt doctest)

add_test(NAME opt_tests COMMAND opt_tests)
set_tests_properties(opt_tests PROPERTIES TIMEOUT 60)

# Library code must not print, read files or use <regex> (root CLAUDE.md hard
# rules). Reuses common's checker over every library source and header.
file(GLOB_RECURSE OPT_LIBRARY_FILES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/stats/*.h ${CMAKE_CURRENT_SOURCE_DIR}/stats/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/selectivity/*.h ${CMAKE_CURRENT_SOURCE_DIR}/selectivity/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rewrite/*.h ${CMAKE_CURRENT_SOURCE_DIR}/rewrite/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/cost/*.h ${CMAKE_CURRENT_SOURCE_DIR}/cost/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/physical/*.h ${CMAKE_CURRENT_SOURCE_DIR}/physical/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/enumerate/*.h ${CMAKE_CURRENT_SOURCE_DIR}/enumerate/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/planner/*.h ${CMAKE_CURRENT_SOURCE_DIR}/planner/*.cpp)
add_test(NAME opt_library_has_no_io_or_regex
    COMMAND ${CMAKE_COMMAND} "-DFILES=${OPT_LIBRARY_FILES}"
            -P ${PROJECT_SOURCE_DIR}/modules/common/tests/check_forbidden.cmake)
