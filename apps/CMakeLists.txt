# --- apps/: thin main() files plus their CLI support libraries ---------------
# All printing and all output JSON live here, never in the module libraries.

# storage CLI (storage chunk 14): the commands as a function, so tests can
# run them with string streams.
add_library(storage_cli_support STATIC storage_cli.cpp)
target_include_directories(storage_cli_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(storage_cli_support PUBLIC db_storage nlohmann_json)

add_executable(storage storage_main.cpp)
target_link_libraries(storage PRIVATE storage_cli_support)

# exec CLI (exec chunk 14): `exec run` and `exec trace`, the same pattern.
add_library(exec_cli_support STATIC exec_cli.cpp)
target_include_directories(exec_cli_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(exec_cli_support PUBLIC db_exec nlohmann_json)

add_executable(exec exec_main.cpp)
target_link_libraries(exec PRIVATE exec_cli_support)

# sql CLI: `sql parse` (sql chunk 05), `compile` and `check` (chunk 12).
# Storage is linked here, not in db_sql: the tool reads the catalog and hands
# it to sql as a TableLookup.
add_library(sql_cli_support STATIC sql_cli.cpp)
target_include_directories(sql_cli_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sql_cli_support PUBLIC db_sql db_storage)

add_executable(sql sql_main.cpp)
target_link_libraries(sql PRIVATE sql_cli_support)

# opt CLI (opt chunk 14): `opt analyze`, `plan`, `explain`, `dp-table`. The
# tool reads the logical plan file and the catalog; db_opt only computes.
add_library(opt_cli_support STATIC opt_cli.cpp)
target_include_directories(opt_cli_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(opt_cli_support PUBLIC db_opt nlohmann_json)

add_executable(opt opt_main.cpp)
target_link_libraries(opt PRIVATE opt_cli_support)

# The shell (integration): one program with a prompt. All of it is in db_shell.
add_executable(shell shell_main.cpp)
target_link_libraries(shell PRIVATE db_shell)

# --- Tests ------------------------------------------------------------------
add_executable(storage_cli_tests
    tests/test_main.cpp
    tests/storage_cli_tests.cpp
)
target_link_libraries(storage_cli_tests PRIVATE storage_cli_support doctest)

add_test(NAME storage_cli_tests COMMAND storage_cli_tests)
set_tests_properties(storage_cli_tests PROPERTIES TIMEOUT 60)

add_executable(exec_cli_tests
    tests/test_main.cpp
    tests/exec_cli_tests.cpp
)
target_link_libraries(exec_cli_tests PRIVATE exec_cli_support doctest)

add_test(NAME exec_cli_tests COMMAND exec_cli_tests)
set_tests_properties(exec_cli_tests PROPERTIES TIMEOUT 60)

add_executable(sql_cli_tests
    tests/test_main.cpp
    tests/sql_cli_tests.cpp
)
target_link_libraries(sql_cli_tests PRIVATE sql_cli_support doctest)

add_test(NAME sql_cli_tests COMMAND sql_cli_tests)
set_tests_properties(sql_cli_tests PROPERTIES TIMEOUT 60)

# The opt tests also run exec's CLI: a plan from `opt plan` must run in exec
# unedited (§M4.6).
add_executable(opt_cli_tests
    tests/test_main.cpp
    tests/opt_cli_tests.cpp
)
target_link_libraries(opt_cli_tests PRIVATE opt_cli_support exec_cli_support doctest)

add_test(NAME opt_cli_tests COMMAND opt_cli_tests)
set_tests_properties(opt_cli_tests PROPERTIES TIMEOUT 60)
