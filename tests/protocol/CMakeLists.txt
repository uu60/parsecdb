# Three-party protocol correctness tests (layer: protocol). Every program in this directory is built and registered
# with parsec_add_party_test (tests/CMakeLists.txt). A test passes when every rank exits with status 0 and no rank
# prints FAIL or MISMATCH.
file(GLOB PARSEC_PROTOCOL_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
foreach (src ${PARSEC_PROTOCOL_TEST_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE parsecdb)
endforeach ()

# Layers below the secure operators do not depend on the BMT source: one test per backend, no BMT suffix.
# parsec_add_backend_tests(<group> <case> <target> [ARGS...])
function(parsec_add_backend_tests group case target)
    foreach (backend IN ITEMS mpi tcp)
        parsec_add_party_test(NAME protocol.${group}.${case}.${backend} TARGET ${target} BACKEND ${backend}
                LABELS protocol ${group} ARGS ${ARGN})
    endforeach ()
endfunction()

# Comm layer: send/receive between every pair of ranks, plain and with transfer compression.
parsec_add_party_test(NAME protocol.comm.send_receive.mpi TARGET correctness_comm BACKEND mpi LABELS protocol comm)
parsec_add_party_test(NAME protocol.comm.send_receive.tcp TARGET correctness_comm BACKEND tcp LABELS protocol comm)

# Oblivious transfer: base OT between the servers and the IKNP extension.
parsec_add_backend_tests(ot base correctness_ot)
parsec_add_backend_tests(ot iknp correctness_iknp_ot)

# BMT generation: bitwise and arithmetic triples.
parsec_add_backend_tests(bmt generation correctness_bmt)

# Secure operators, with every BMT source over MPI and the default source over TCP.
set(PARSEC_OPERATOR_VARIANTS mpi.jit mpi.fixed mpi.background tcp.jit)

# parsec_add_operator_tests(<group> <case> <target> [SLOW] [ARGS <args...>] [VARIANTS <backend.bmt...>])
function(parsec_add_operator_tests group case target)
    cmake_parse_arguments(T "SLOW" "" "ARGS;VARIANTS" ${ARGN})
    if (NOT T_VARIANTS)
        set(T_VARIANTS ${PARSEC_OPERATOR_VARIANTS})
    endif ()
    set(extra "")
    if (T_SLOW)
        list(APPEND extra SLOW)
    endif ()
    parsec_add_bmt_variants(NAME protocol.${group}.${case} TARGET ${target} LABELS protocol ${group}
            VARIANTS ${T_VARIANTS} ARGS ${T_ARGS} ${extra})
endfunction()


parsec_add_operator_tests(boolean and_xor correctness_bool_and_xor)
parsec_add_operator_tests(boolean equal correctness_bool_equal)
parsec_add_operator_tests(boolean less correctness_bool_less)
parsec_add_operator_tests(boolean mux correctness_bool_mux)

parsec_add_operator_tests(arithmetic add_multiply correctness_arith_add_multiply)
parsec_add_operator_tests(arithmetic less correctness_arith_less)
parsec_add_operator_tests(arithmetic mux correctness_arith_mux)

parsec_add_operator_tests(conversion a2b_b2a correctness_share_conversion)

parsec_add_operator_tests(secret sort_arith correctness_secrets_sort SLOW ARGS --test_mode=arith)
parsec_add_operator_tests(secret sort_bool correctness_secrets_sort SLOW ARGS --test_mode=bool)
