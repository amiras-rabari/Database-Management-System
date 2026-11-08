# --- db_exec: iterator operators over the storage engine --------------------
add_library(db_exec STATIC
    budget/frame_plan.cpp
    budget/tuple_buffer.cpp
    operators/operator.cpp
    operators/pipelined.cpp
    plan/executable_plan.cpp
    sort/external_sort.cpp
    sort/run_file.cpp
    join/hash_partitioner.cpp
    join/hashing.cpp
    join/nested_loop_join.cpp
    join/tuple_hash_table.cpp
    operators/hash_group.cpp
    operators/grouping.cpp
    sort/set_ops.cpp
    sort/sort_operator.cpp
)
target_include_directories(db_exec PUBLIC include)
# Private headers (operators/pipelined.h, ...) are included as "operators/<name>.h".
target_include_directories(db_exec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Exec may use common and storage only (dependency rule in the root CLAUDE.md).
# db_storage brings db_common with it.
target_link_libraries(db_exec PUBLIC db_storage)

# --- Tests ------------------------------------------------------------------
add_executable(exec_tests
    tests/test_main.cpp
    tests/external_sort_tests.cpp
    tests/set_ops_tests.cpp
    tests/hash_distinct_tests.cpp
    tests/group_by_tests.cpp
    tests/nested_loop_join_tests.cpp
    tests/frame_plan_tests.cpp
    tests/operator_tests.cpp
    tests/pipelined_tests.cpp
    tests/plan_builder_tests.cpp
    tests/run_file_tests.cpp
    tests/tuple_buffer_tests.cpp
)
target_link_libraries(exec_tests PRIVATE db_exec doctest)
target_include_directories(exec_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME exec_tests COMMAND exec_tests)
set_tests_properties(exec_tests PROPERTIES TIMEOUT 60)
