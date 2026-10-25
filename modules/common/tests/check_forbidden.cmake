# Fails if any library file prints, reads files, or uses <regex>
# (CLAUDE.md hard rules; Phase 0 point 1). Run by ctest with -DFILES=a;b;...
set(FORBIDDEN
    "std::cout" "std::cerr" "std::clog" "std::cin"
    "<iostream>" "<fstream>" "<cstdio>" "<stdio.h>" "<regex>"
    "printf(" "puts(" "fopen("
)

set(found FALSE)
foreach(file ${FILES})
    # Whole-file search: file(STRINGS) would split C++ lines at every ';'.
    file(READ ${file} content)
    foreach(pattern ${FORBIDDEN})
        string(FIND "${content}" "${pattern}" position)
        if(NOT position EQUAL -1)
            message(STATUS "forbidden '${pattern}' in ${file}")
            set(found TRUE)
        endif()
    endforeach()
endforeach()

if(found)
    message(FATAL_ERROR "library code must not print, read files or use <regex>")
endif()
