# --- db_sql: the SQL front end (lexer, parser, semantics, translator) --------
set(DB_SQL_SOURCES
    lexer/sql_lexer.cpp
)
add_library(db_sql STATIC ${DB_SQL_SOURCES})
target_include_directories(db_sql PUBLIC include)
# sql links only common. The storage catalog reaches sql through a function
# object (chunk 07), so only apps/ link storage.
target_link_libraries(db_sql PUBLIC db_common)

# --- Tests ------------------------------------------------------------------
add_executable(sql_tests
    tests/test_main.cpp
    tests/lexer_tests.cpp
    tests/ast_tests.cpp
)
target_link_libraries(sql_tests PRIVATE db_sql doctest)

add_test(NAME sql_tests COMMAND sql_tests)
set_tests_properties(sql_tests PROPERTIES TIMEOUT 60)

# Library code must not print, read files or use <regex> (root CLAUDE.md hard
# rules). Reuses common's checker over every library source and header.
file(GLOB_RECURSE SQL_LIBRARY_FILES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/lexer/*.h ${CMAKE_CURRENT_SOURCE_DIR}/lexer/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/parser/*.h ${CMAKE_CURRENT_SOURCE_DIR}/parser/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ast/*.h ${CMAKE_CURRENT_SOURCE_DIR}/ast/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/semantic/*.h ${CMAKE_CURRENT_SOURCE_DIR}/semantic/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/translate/*.h ${CMAKE_CURRENT_SOURCE_DIR}/translate/*.cpp)
add_test(NAME sql_library_has_no_io_or_regex
    COMMAND ${CMAKE_COMMAND} "-DFILES=${SQL_LIBRARY_FILES}"
            -P ${PROJECT_SOURCE_DIR}/modules/common/tests/check_forbidden.cmake)
