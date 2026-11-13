# --- db_shell: the one program with a prompt (integration) ------------------
# SQL in, rows out: sql compiles, opt plans, exec runs, storage reads pages.
# The shell only wires the module libraries together.
add_library(db_shell STATIC
    src/shell.cpp
    src/statements.cpp
)
target_include_directories(db_shell PUBLIC include)
# The shell may use every module (root CLAUDE.md). db_opt brings db_exec,
# db_storage and db_common; they are listed so the wiring is visible.
target_link_libraries(db_shell PUBLIC db_sql db_opt db_exec db_storage db_common nlohmann_json)

# --- Tests ------------------------------------------------------------------
add_executable(shell_tests
    tests/test_main.cpp
    tests/shell_tests.cpp
)
target_link_libraries(shell_tests PRIVATE db_shell doctest)

add_test(NAME shell_tests COMMAND shell_tests)
set_tests_properties(shell_tests PROPERTIES TIMEOUT 60)

# The `shell` executable itself (built in apps/): exit codes 0, 1 and 2 on
# small scripts (chunk 03 test 5).
add_test(NAME shell_executable_exit_codes
    COMMAND ${CMAKE_COMMAND}
            -DSHELL=$<TARGET_FILE:shell>
            -DSCRIPTS=${CMAKE_CURRENT_SOURCE_DIR}/tests
            -DDIR=${CMAKE_CURRENT_BINARY_DIR}/shell_smoke_db
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/shell_exit_codes.cmake)
