cmake_minimum_required(VERSION 3.20)

project(ucn_core VERSION 4.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(UCN_BUILD_TESTS "Build UCN unit and simulation tests" ON)

add_library(ucn_core STATIC
    src/ucn_core.c
    src/ucn_adapter.c
    src/ucn_endpoint.c
    src/ucn_frame.c
    src/ucn_node.c
    src/ucn_path.c
    src/ucn_policy.c
    src/ucn_service.c
    src/ucn_service_bridge.c
)

target_include_directories(ucn_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(ucn_core PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

if(UCN_BUILD_TESTS)
    enable_testing()

    add_executable(ucn_tests
        tests/test_main.c
        tests/test_core.c
        tests/test_frame.c
        tests/test_node.c
        tests/test_virtual_link.c
        tests/test_qos.c
        tests/test_route.c
        tests/test_aodv_lite.c
        tests/test_security.c
        tests/test_host_boundary.c
        tests/test_integration.c
        tests/test_link_metrics.c
        tests/test_neighbor_lifecycle.c
        tests/test_hello_join.c
        tests/test_neighbor_heartbeat.c
        tests/test_neighbor_bearer.c
        tests/test_endpoint.c
        tests/test_candidate_route.c
        tests/test_adapter.c
        tests/test_v3.c
        tests/test_path_trace.c
        tests/test_node_snapshot.c
        tests/test_path_control.c
        tests/test_policy.c
        tests/test_policy_diagnostic.c
        tests/test_service.c
        tests/test_service_bridge.c
    )

    target_link_libraries(ucn_tests PRIVATE ucn_core)

    target_compile_options(ucn_tests PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    )

    add_test(NAME ucn_tests COMMAND ucn_tests)
endif()
