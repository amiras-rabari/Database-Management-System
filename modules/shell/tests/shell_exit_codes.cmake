# Runs the real `shell` program three times and checks its exit code.
# Called by CTest with -DSHELL=<program> -DSCRIPTS=<tests folder> -DDIR=<database folder>.

# A fresh database folder every run, so CREATE TABLE in smoke.sql succeeds.
file(REMOVE_RECURSE ${DIR})

# 1. Every statement works -> 0.
execute_process(COMMAND ${SHELL} --db ${DIR} --file ${SCRIPTS}/smoke.sql
                RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 0)
    message(FATAL_ERROR "smoke.sql: expected exit 0, got ${code}\n${out}\n${err}")
endif()
if(NOT out MATCHES "2 rows")
    message(FATAL_ERROR "smoke.sql: expected '2 rows' in the output, got:\n${out}")
endif()

# 2. One statement fails -> 1 (the others still run).
execute_process(COMMAND ${SHELL} --db ${DIR} --file ${SCRIPTS}/failing.sql
                RESULT_VARIABLE code OUTPUT_QUIET ERROR_QUIET)
if(NOT code EQUAL 1)
    message(FATAL_ERROR "failing.sql: expected exit 1, got ${code}")
endif()

# 3. B below 3 on the command line is a usage error -> 2.
execute_process(COMMAND ${SHELL} --db ${DIR} --frames 2 --file ${SCRIPTS}/smoke.sql
                RESULT_VARIABLE code OUTPUT_QUIET ERROR_QUIET)
if(NOT code EQUAL 2)
    message(FATAL_ERROR "--frames 2: expected exit 2, got ${code}")
endif()

file(REMOVE_RECURSE ${DIR})
