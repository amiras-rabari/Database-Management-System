# --- apps/: thin main() files plus their CLI support libraries ---------------
# All printing and all output JSON live here, never in the module libraries.

# storage CLI (storage chunk 14): the commands as a function, so tests can
# run them with string streams.
add_library(storage_cli_support STATIC storage_cli.cpp)
target_include_directories(storage_cli_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(storage_cli_support PUBLIC db_storage nlohmann_json)

add_executable(storage storage_main.cpp)
target_link_libraries(storage PRIVATE storage_cli_support)

# --- Tests ------------------------------------------------------------------
add_executable(storage_cli_tests
    tests/test_main.cpp
    tests/storage_cli_tests.cpp
)
target_link_libraries(storage_cli_tests PRIVATE storage_cli_support doctest)

add_test(NAME storage_cli_tests COMMAND storage_cli_tests)
set_tests_properties(storage_cli_tests PROPERTIES TIMEOUT 60)
