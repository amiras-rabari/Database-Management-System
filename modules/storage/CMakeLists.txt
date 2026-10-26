# --- db_storage: the only library that touches files ------------------------
set(DB_STORAGE_SOURCES
    disk/bytes.cpp
    disk/disk_manager.cpp
    page/record_codec.cpp
    page/slotted_page.cpp
    buffer/replacement_policy.cpp
    buffer/replacer.cpp
    buffer/lru_replacer.cpp
    buffer/clock_replacer.cpp
)
set(DB_STORAGE_HEADERS
    include/db/storage/page.h
    include/db/storage/bytes.h
    include/db/storage/disk_manager.h
    include/db/storage/record_codec.h
    include/db/storage/slotted_page.h
    include/db/storage/replacement_policy.h
)
add_library(db_storage STATIC ${DB_STORAGE_SOURCES})
target_include_directories(db_storage PUBLIC include)
# Module-private headers (e.g. "buffer/replacer.h") are included relative to
# this folder; other modules never see them.
target_include_directories(db_storage PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Storage may use common only (dependency rule in the root CLAUDE.md).
target_link_libraries(db_storage PUBLIC db_common)

# Every public header must compile on its own (same check as db_common).
set(HEADER_CHECK_SOURCES)
foreach(header ${DB_STORAGE_HEADERS})
    set(tu ${CMAKE_CURRENT_BINARY_DIR}/header_check/${header}.cpp)
    file(GENERATE OUTPUT ${tu} CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/${header}\"\n")
    list(APPEND HEADER_CHECK_SOURCES ${tu})
endforeach()
add_library(db_storage_header_check OBJECT ${HEADER_CHECK_SOURCES})
target_link_libraries(db_storage_header_check PRIVATE db_storage)

# --- Tests ------------------------------------------------------------------
add_executable(storage_tests
    tests/test_main.cpp
    tests/bytes_tests.cpp
    tests/disk_manager_tests.cpp
    tests/record_codec_tests.cpp
    tests/slotted_page_tests.cpp
    tests/replacer_tests.cpp
)
target_link_libraries(storage_tests PRIVATE db_storage doctest)
# The module's own tests may use its private headers.
target_include_directories(storage_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME storage_tests COMMAND storage_tests)
set_tests_properties(storage_tests PROPERTIES TIMEOUT 60)
