# Runs exp_1..exp_8 in --check=true mode over MPI and TCP, and with background BMTs over MPI, and compares their output
# with the expected results in verify_exp.py.
find_package(Python3 COMPONENTS Interpreter)
if (NOT Python3_Interpreter_FOUND)
    message(STATUS "Python 3 not found - database checks will not be registered")
    return()
endif ()

set(PARSEC_VERIFY_EXP "${CMAKE_CURRENT_SOURCE_DIR}/verify_exp.py")
foreach (exp RANGE 1 8)
    add_test(NAME database.exp_${exp}.mpi
            COMMAND ${Python3_EXECUTABLE} ${PARSEC_VERIFY_EXP} --exp ${exp} --comm mpi
                    --mpirun ${MPIEXEC_EXECUTABLE} "--mpirun-flags=${PARSEC_MPIEXEC_TEST_FLAGS}"
                    --bin-dir $<TARGET_FILE_DIR:exp_${exp}>)
    add_test(NAME database.exp_${exp}.tcp
            COMMAND ${Python3_EXECUTABLE} ${PARSEC_VERIFY_EXP} --exp ${exp} --comm tcp
                    --tcp-base-port 21000 --bin-dir $<TARGET_FILE_DIR:exp_${exp}>)
    set_tests_properties(database.exp_${exp}.mpi PROPERTIES
            LABELS "correctness;database;mpi" PROCESSORS 3 TIMEOUT 180)
    if (PARSEC_MPI_TEST_ENVIRONMENT)
        set_property(TEST database.exp_${exp}.mpi PROPERTY ENVIRONMENT ${PARSEC_MPI_TEST_ENVIRONMENT})
    endif ()
    set_tests_properties(database.exp_${exp}.tcp PROPERTIES
            LABELS "correctness;database;tcp" PROCESSORS 3 TIMEOUT 180)

    # The same check with BMTs from the background generator queues, which exercises the ordered BMT hand-off to
    # parallel operators and the coordinated generator shutdown.
    add_test(NAME database.exp_${exp}.mpi.background
            COMMAND ${Python3_EXECUTABLE} ${PARSEC_VERIFY_EXP} --exp ${exp} --comm mpi
                    --mpirun ${MPIEXEC_EXECUTABLE} "--mpirun-flags=${PARSEC_MPIEXEC_TEST_FLAGS}"
                    --bmt-method bmt_background --max-bmts 10000 --bin-dir $<TARGET_FILE_DIR:exp_${exp}>)
    set_tests_properties(database.exp_${exp}.mpi.background PROPERTIES
            LABELS "correctness;database;mpi;background" PROCESSORS 3 TIMEOUT 180)
    if (PARSEC_MPI_TEST_ENVIRONMENT)
        set_property(TEST database.exp_${exp}.mpi.background PROPERTY ENVIRONMENT ${PARSEC_MPI_TEST_ENVIRONMENT})
    endif ()
endforeach ()

# Grouped aggregates checked against plaintext, with single-batch and multi-batch execution in one run.
add_executable(correctness_db_aggregates ${CMAKE_CURRENT_SOURCE_DIR}/correctness_db_aggregates.cpp)
target_link_libraries(correctness_db_aggregates PRIVATE parsecdb::db)
foreach (variant IN ITEMS jit background background_single_thread)
    set(args "")
    if (variant STREQUAL "background")
        set(args --bmt_method=bmt_background --disable_arith=false --max_bmts=10000)
    elseif (variant STREQUAL "background_single_thread")
        # Operators on one thread while the BMT generators run on their own threads.
        set(args --bmt_method=bmt_background --disable_arith=false --max_bmts=10000 --disable_multi_thread=true)
    endif ()
    add_test(NAME database.aggregates.mpi.${variant}
            COMMAND ${MPIEXEC_EXECUTABLE} ${PARSEC_MPIEXEC_TEST_FLAGS_LIST} ${MPIEXEC_NUMPROC_FLAG} 3
                    ${MPIEXEC_PREFLAGS} $<TARGET_FILE:correctness_db_aggregates> ${args} ${MPIEXEC_POSTFLAGS})
    set_tests_properties(database.aggregates.mpi.${variant} PROPERTIES
            LABELS "correctness;database;mpi" PROCESSORS 3 TIMEOUT 180 FAIL_REGULAR_EXPRESSION "FAIL")
    if (PARSEC_MPI_TEST_ENVIRONMENT)
        set_property(TEST database.aggregates.mpi.${variant} PROPERTY ENVIRONMENT ${PARSEC_MPI_TEST_ENVIRONMENT})
    endif ()
endforeach ()
