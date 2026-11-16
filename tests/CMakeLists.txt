# CTest suite, layered in dependency order. A test name is <layer>.<group>.<case>.<backend>[.<bmt>] and its labels are
# layer;group;backend[;bmt] plus correctness (and slow for the long ones), so any slice can be selected with -L:
#
#   unit.<Suite>.<Case>                  single-process GoogleTest cases (label: unit)
#   protocol.comm.*                      Comm layer send/receive over MPI and TCP
#   protocol.ot.*, protocol.bmt.*        base OT, IKNP OT extension and BMT generation
#   protocol.{boolean,arithmetic,conversion,secret}.*
#                                        secure operators with every BMT source (jit, fixed, background)
#   db.operators.*                       database operators against plaintext (needs PARSEC_BUILD_DATABASE)
#   query.exp_<N>.*                      experiment queries in check mode (needs PARSEC_BUILD_EXPERIMENTS)
#
# For example: ctest -L protocol -L background, ctest -L tcp, ctest -LE slow.

find_package(Python3 COMPONENTS Interpreter)

# TCP tests run three local processes on ports BASE..BASE+2. Every TCP test gets its own block of ports so tests can
# run in parallel. The counter is a global property because tests are registered from several directories.
set(PARSEC_TCP_TEST_FIRST_PORT 19000 CACHE STRING "First local port used by the TCP tests")
set(PARSEC_TCP_TEST_PORT_STRIDE 10)
set_property(GLOBAL PROPERTY PARSEC_TCP_TEST_NEXT_PORT ${PARSEC_TCP_TEST_FIRST_PORT})

# parsec_allocate_tcp_port(<out-var>): reserves the next block of PARSEC_TCP_TEST_PORT_STRIDE ports.
function(parsec_allocate_tcp_port out)
    get_property(port GLOBAL PROPERTY PARSEC_TCP_TEST_NEXT_PORT)
    math(EXPR next "${port} + ${PARSEC_TCP_TEST_PORT_STRIDE}")
    set_property(GLOBAL PROPERTY PARSEC_TCP_TEST_NEXT_PORT ${next})
    set(${out} ${port} PARENT_SCOPE)
endfunction()

# parsec_bmt_args(<variant> <out-var>): program arguments for a BMT source. Background mode also generates arithmetic
# BMTs and bounds the queues; background_single_thread runs the operators on one thread while the generators keep
# their own threads.
function(parsec_bmt_args variant out)
    set(background --bmt_method=bmt_background --disable_arith=false --max_bmts=10000)
    if (variant STREQUAL "jit")
        set(args --bmt_method=bmt_jit)
    elseif (variant STREQUAL "fixed")
        set(args --bmt_method=bmt_fixed)
    elseif (variant STREQUAL "background")
        set(args ${background})
    elseif (variant STREQUAL "background_single_thread")
        set(args ${background} --disable_multi_thread=true)
    else ()
        message(FATAL_ERROR "Unknown BMT variant '${variant}'")
    endif ()
    set(${out} ${args} PARENT_SCOPE)
endfunction()

# parsec_add_party_test(NAME <name> BACKEND mpi|tcp LABELS <labels...>
#                       {TARGET <target> [ARGS <args...>] | COMMAND <command...>}
#                       [TIMEOUT <seconds>] [FAIL_REGEX <regex...> | NO_FAIL_REGEX] [SLOW])
#
# Registers a three-rank test. With TARGET, the executable is started as three MPI ranks with mpiexec, or as three
# local TCP processes with tools/run_tcp_ranks.py on a freshly allocated port block. With COMMAND, the command starts
# the ranks itself (a TCP command takes its ports from parsec_allocate_tcp_port). The test fails when it exits non-zero
# or prints a line matching FAIL_REGEX (default "FAIL;MISMATCH"; NO_FAIL_REGEX checks the exit status only). LABELS
# get the backend and correctness appended; SLOW adds the slow label and a longer default timeout.
function(parsec_add_party_test)
    cmake_parse_arguments(T "SLOW;NO_FAIL_REGEX" "NAME;TARGET;BACKEND;TIMEOUT" "LABELS;ARGS;COMMAND;FAIL_REGEX" ${ARGN})
    if (NOT T_NAME OR NOT T_BACKEND OR (NOT T_TARGET AND NOT T_COMMAND))
        message(FATAL_ERROR "parsec_add_party_test: NAME, BACKEND and TARGET or COMMAND are required")
    endif ()
    if (NOT T_BACKEND MATCHES "^(mpi|tcp)$")
        message(FATAL_ERROR "parsec_add_party_test(${T_NAME}): BACKEND must be mpi or tcp")
    endif ()
    if (NOT T_TIMEOUT)
        if (T_SLOW)
            set(T_TIMEOUT 300)
        else ()
            set(T_TIMEOUT 120)
        endif ()
    endif ()
    if (T_NO_FAIL_REGEX)
        set(T_FAIL_REGEX "")
    elseif (NOT T_FAIL_REGEX)
        set(T_FAIL_REGEX "FAIL;MISMATCH")
    endif ()

    if (T_COMMAND)
        set(command ${T_COMMAND})
    elseif (T_BACKEND STREQUAL "mpi")
        set(command ${MPIEXEC_EXECUTABLE} ${PARSEC_MPIEXEC_TEST_FLAGS_LIST} ${MPIEXEC_NUMPROC_FLAG} 3
                ${MPIEXEC_PREFLAGS} $<TARGET_FILE:${T_TARGET}> ${T_ARGS} ${MPIEXEC_POSTFLAGS})
    else ()
        if (NOT Python3_Interpreter_FOUND)
            return()
        endif ()
        parsec_allocate_tcp_port(port)
        # The runner kills the ranks a little before CTest would, so their output is still reported.
        math(EXPR runner_timeout "${T_TIMEOUT} - 10")
        set(command ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/run_tcp_ranks.py
                --base-port ${port} --timeout ${runner_timeout} $<TARGET_FILE:${T_TARGET}> -- ${T_ARGS})
    endif ()

    add_test(NAME ${T_NAME} COMMAND ${command})
    set(labels ${T_LABELS} ${T_BACKEND} correctness)
    if (T_SLOW)
        list(APPEND labels slow)
    endif ()
    list(REMOVE_DUPLICATES labels)
    set_tests_properties(${T_NAME} PROPERTIES
            LABELS "${labels}"
            PROCESSORS 3
            TIMEOUT ${T_TIMEOUT})
    if (T_FAIL_REGEX)
        set_property(TEST ${T_NAME} PROPERTY FAIL_REGULAR_EXPRESSION "${T_FAIL_REGEX}")
    endif ()
    if (T_BACKEND STREQUAL "mpi" AND PARSEC_MPI_TEST_ENVIRONMENT)
        set_property(TEST ${T_NAME} PROPERTY ENVIRONMENT ${PARSEC_MPI_TEST_ENVIRONMENT})
    endif ()
endfunction()

# parsec_add_bmt_variants(NAME <layer.group.case> TARGET <target> LABELS <labels...>
#                         VARIANTS <backend.bmt...> [ARGS <args...>] [TIMEOUT <seconds>] [FAIL_REGEX <regex>] [SLOW])
#
# One parsec_add_party_test per VARIANTS entry, e.g. mpi.jit or tcp.background, named <NAME>.<backend>.<bmt> and
# labelled with the BMT source in addition to LABELS.
function(parsec_add_bmt_variants)
    cmake_parse_arguments(T "SLOW" "NAME;TARGET;TIMEOUT" "LABELS;VARIANTS;ARGS;FAIL_REGEX" ${ARGN})
    set(extra "")
    if (T_SLOW)
        list(APPEND extra SLOW)
    endif ()
    if (T_TIMEOUT)
        list(APPEND extra TIMEOUT ${T_TIMEOUT})
    endif ()
    if (T_FAIL_REGEX)
        list(APPEND extra FAIL_REGEX ${T_FAIL_REGEX})
    endif ()
    foreach (variant IN LISTS T_VARIANTS)
        if (NOT variant MATCHES "^(mpi|tcp)\\.([a-z_]+)$")
            message(FATAL_ERROR "parsec_add_bmt_variants(${T_NAME}): bad variant '${variant}'")
        endif ()
        set(backend ${CMAKE_MATCH_1})
        set(bmt ${CMAKE_MATCH_2})
        parsec_bmt_args(${bmt} bmt_args)
        parsec_add_party_test(NAME ${T_NAME}.${variant} TARGET ${T_TARGET} BACKEND ${backend}
                LABELS ${T_LABELS} ${bmt} ARGS ${bmt_args} ${T_ARGS} ${extra})
    endforeach ()
endfunction()

add_subdirectory(unit)
add_subdirectory(protocol)
if (TARGET parsecdb::db)
    add_subdirectory(database)
endif ()
