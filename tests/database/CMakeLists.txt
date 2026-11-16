# Database layers, registered with the helpers in tests/CMakeLists.txt:
#   db.operators.<backend>.<bmt>   database operators checked against plaintext (correctness_db_operators)
#   query.exp_<N>.<backend>.<bmt>  exp_1..exp_8 in --check=true mode, compared with the expected results in
#                                  verify_exp.py (only when the experiments are built)

add_executable(correctness_db_operators ${CMAKE_CURRENT_SOURCE_DIR}/correctness_db_operators.cpp)
target_link_libraries(correctness_db_operators PRIVATE parsecdb::db)
parsec_add_bmt_variants(NAME db.operators TARGET correctness_db_operators LABELS db operators TIMEOUT 180
        VARIANTS mpi.jit mpi.fixed mpi.background mpi.background_single_thread tcp.jit
        FAIL_REGEX "FAIL")

if (NOT TARGET exp_1)
    return()
endif ()
if (NOT Python3_Interpreter_FOUND)
    message(STATUS "Python 3 not found - query checks will not be registered")
    return()
endif ()

set(PARSEC_VERIFY_EXP "${CMAKE_CURRENT_SOURCE_DIR}/verify_exp.py")
foreach (exp RANGE 1 8)
    foreach (variant IN ITEMS mpi.jit mpi.background tcp.jit)
        string(REPLACE "." ";" parts ${variant})
        list(GET parts 0 backend)
        list(GET parts 1 bmt)
        set(command ${Python3_EXECUTABLE} ${PARSEC_VERIFY_EXP} --exp ${exp} --comm ${backend}
                --bin-dir $<TARGET_FILE_DIR:exp_${exp}> --bmt-method bmt_${bmt})
        if (bmt STREQUAL "background")
            list(APPEND command --max-bmts 10000)
        endif ()
        if (backend STREQUAL "mpi")
            list(APPEND command --mpirun ${MPIEXEC_EXECUTABLE} "--mpirun-flags=${PARSEC_MPIEXEC_TEST_FLAGS}")
        else ()
            # verify_exp.py listens on --tcp-base-port + 10 * exp; aim that at this test's own port block.
            parsec_allocate_tcp_port(port)
            math(EXPR base "${port} - 10 * ${exp}")
            list(APPEND command --tcp-base-port ${base})
        endif ()
        parsec_add_party_test(NAME query.exp_${exp}.${variant} BACKEND ${backend} COMMAND ${command}
                LABELS query exp_${exp} ${bmt} TIMEOUT 180 NO_FAIL_REGEX)
    endforeach ()
endforeach ()
