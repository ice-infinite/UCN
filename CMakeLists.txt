cmake_minimum_required(VERSION 3.20)

project(ucn_core VERSION 5.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(UCN_BUILD_TESTS "Build UCN unit and simulation tests" ON)
option(UCN_BUILD_SCALE_SIM "Build the host-only UCN scale simulator" ON)
set(UCN_PROFILE "FULL" CACHE STRING "UCN feature profile: NANO, LITE, or FULL")
set_property(CACHE UCN_PROFILE PROPERTY STRINGS NANO LITE FULL)
option(UCN_FEATURE_SERVICE "Build the orthogonal Service Router/Bridge" ON)

string(TOUPPER "${UCN_PROFILE}" UCN_PROFILE_UPPER)
if(UCN_PROFILE_UPPER STREQUAL "NANO")
    set(UCN_PROFILE_VALUE 1)
elseif(UCN_PROFILE_UPPER STREQUAL "LITE")
    set(UCN_PROFILE_VALUE 2)
elseif(UCN_PROFILE_UPPER STREQUAL "FULL")
    set(UCN_PROFILE_VALUE 3)
else()
    message(FATAL_ERROR "UCN_PROFILE must be NANO, LITE, or FULL")
endif()

message(STATUS "UCN profile: ${UCN_PROFILE_UPPER}; Service: ${UCN_FEATURE_SERVICE}")

set(UCN_CORE_SOURCES
    src/ucn_core.c
    src/ucn_adapter.c
    src/ucn_endpoint.c
    src/ucn_frame.c
)

if(UCN_PROFILE_UPPER STREQUAL "NANO")
    list(APPEND UCN_CORE_SOURCES src/ucn_node_nano.c)
else()
    list(APPEND UCN_CORE_SOURCES src/ucn_node.c)
endif()

if(NOT UCN_PROFILE_UPPER STREQUAL "FULL")
    list(APPEND UCN_CORE_SOURCES src/ucn_profile_stubs.c)
endif()

if(UCN_PROFILE_UPPER STREQUAL "FULL")
    list(APPEND UCN_CORE_SOURCES
        src/ucn_path.c
        src/ucn_policy.c
    )
endif()

if(UCN_FEATURE_SERVICE)
    list(APPEND UCN_CORE_SOURCES
        src/ucn_service.c
        src/ucn_service_bridge.c
    )
    set(UCN_FEATURE_SERVICE_VALUE 1)
else()
    set(UCN_FEATURE_SERVICE_VALUE 0)
endif()

add_library(ucn_core STATIC ${UCN_CORE_SOURCES})

target_include_directories(ucn_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(ucn_core
    PUBLIC
        UCN_PROFILE=${UCN_PROFILE_VALUE}
        UCN_FEATURE_SERVICE=${UCN_FEATURE_SERVICE_VALUE}
)

target_compile_options(ucn_core PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

if(UCN_BUILD_SCALE_SIM AND NOT UCN_PROFILE_UPPER STREQUAL "NANO")
    add_executable(ucn_scale_sim tools/ucn_scale_sim.c)
    target_link_libraries(ucn_scale_sim PRIVATE ucn_core)
    target_compile_options(ucn_scale_sim PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    )
endif()

if(UCN_BUILD_TESTS)
    enable_testing()

    set(UCN_TEST_SOURCES
        tests/test_main.c
        tests/test_core.c
        tests/test_frame.c
        tests/test_wire_profile.c
        tests/test_node_wire_profile.c
        tests/test_node.c
        tests/test_virtual_link.c
        tests/test_qos.c
        tests/test_route.c
        tests/test_duplicate_window.c
        tests/test_host_boundary.c
        tests/test_endpoint.c
        tests/test_adapter.c
        tests/test_profile.c
        tests/test_public_headers.c
        tests/test_node_storage_header.c
    )

    if(NOT UCN_PROFILE_UPPER STREQUAL "NANO")
        list(APPEND UCN_TEST_SOURCES
            tests/test_aodv_lite.c
            tests/test_security.c
            tests/test_integration.c
            tests/test_link_metrics.c
            tests/test_neighbor_lifecycle.c
            tests/test_hello_join.c
            tests/test_neighbor_heartbeat.c
            tests/test_neighbor_bearer.c
            tests/test_adapter_hello.c
            tests/test_control_budget.c
            tests/test_time.c
            tests/test_link_contract.c
            tests/test_stress.c
        )
    endif()

    if(UCN_PROFILE_UPPER STREQUAL "FULL")
        list(APPEND UCN_TEST_SOURCES
            tests/test_candidate_route.c
            tests/test_v3.c
            tests/test_path_trace.c
            tests/test_node_snapshot.c
            tests/test_path_control.c
            tests/test_path_management_budget.c
            tests/test_policy.c
            tests/test_policy_diagnostic.c
            tests/test_dynamic_stress.c
        )
    endif()

    add_executable(ucn_tests ${UCN_TEST_SOURCES})

    if(UCN_FEATURE_SERVICE)
        target_sources(ucn_tests PRIVATE
            tests/test_service.c
            tests/test_service_bridge.c
        )
    endif()

    target_link_libraries(ucn_tests PRIVATE ucn_core)

    target_compile_options(ucn_tests PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    )

    add_test(NAME ucn_tests COMMAND ucn_tests)
    if(TARGET ucn_scale_sim)
        add_test(NAME ucn_scale_smoke COMMAND ucn_scale_sim
            --nodes 8
            --ticks 20
            --warmup-ticks 40
            --drain-ticks 120
            --traffic two-hop
            --quiet
        )
    endif()
endif()
