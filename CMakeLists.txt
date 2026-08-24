cmake_minimum_required(VERSION 3.20)

project(ucn_core VERSION 5.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(UCN_BUILD_TESTS "Build UCN unit and simulation tests" ON)
option(UCN_BUILD_SCALE_SIM "Build the host-only UCN scale simulator" ON)
option(UCN_BUILD_CLUSTER_SIM "Build the host-only UCN Cluster simulator" ON)
option(UCN_BUILD_CONFIG_CONTRACT_TESTS
    "Build standalone global/default/fallback configuration contract tests" OFF)
option(UCN_ENABLE_WIRE_V4_RELEASE_GATES
    "Build test-only optimized RFC4 codec gates (-O1/-O2/-O3 or MSVC equivalents)" OFF)
option(UCN_CLUSTER_ENABLE_V3_COMPAT
    "Build/link the legacy Current-FSM v3 Cluster wire codec" ON)
option(UCN_BUILD_CLUSTER_TAKEOVER_EXPERIMENTAL
    "Build the isolated M10 frozen-majority takeover model (default OFF)" OFF)
option(UCN_BUILD_CLUSTER_HANDOVER_EXPERIMENTAL
    "Build the isolated M11 Merge/Handover model (default OFF)" OFF)
option(UCN_BUILD_CLUSTER_REKEY_EXPERIMENTAL
    "Build the isolated M13 Rekey/No-wrap model (default OFF)" OFF)
set(UCN_PROFILE "FULL" CACHE STRING "UCN feature profile: NANO, LITE, or FULL")
set_property(CACHE UCN_PROFILE PROPERTY STRINGS NANO LITE FULL)
option(UCN_FEATURE_SERVICE "Build the orthogonal Service Router/Bridge" ON)
set(UCN_USER_CONFIG_HEADER "" CACHE STRING
    "Optional product configuration header name included before UCN defaults")
set(UCN_USER_CONFIG_INCLUDE_DIR "" CACHE PATH
    "Optional include directory containing UCN_USER_CONFIG_HEADER")

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

set(UCN_FOUNDATION_SOURCES
    src/core/ucn_core.c
    src/core/ucn_endpoint.c
    src/core/ucn_frame.c
    src/core/ucn_link_cost.c
)

set(UCN_TRANSPORT_SOURCES
    src/transport/ucn_adapter.c
    src/transport/ucn_event_runtime.c
    src/transport/ucn_standard_adapter.c
    src/transport/ucn_protocol_owner.c
)

set(UCN_ADAPTER_SOURCES
    src/adapters/can/ucn_can_source.c
    src/adapters/stream/ucn_stream_source.c
)

set(UCN_NODE_SOURCES)
set(UCN_ROUTING_SOURCES)
set(UCN_SERVICE_SOURCES)

if(UCN_PROFILE_UPPER STREQUAL "NANO")
    list(APPEND UCN_NODE_SOURCES src/node/ucn_node_nano.c)
else()
    list(APPEND UCN_NODE_SOURCES src/node/ucn_node.c)
endif()

if(NOT UCN_PROFILE_UPPER STREQUAL "FULL")
    list(APPEND UCN_NODE_SOURCES src/node/ucn_profile_stubs.c)
endif()

if(UCN_PROFILE_UPPER STREQUAL "FULL")
    list(APPEND UCN_ROUTING_SOURCES
        src/routing/ucn_path.c
        src/routing/ucn_policy.c
    )
endif()

if(UCN_FEATURE_SERVICE)
    list(APPEND UCN_SERVICE_SOURCES
        src/service/ucn_service.c
        src/service/ucn_service_bridge.c
    )
    set(UCN_FEATURE_SERVICE_VALUE 1)
else()
    set(UCN_FEATURE_SERVICE_VALUE 0)
endif()

set(UCN_CORE_SOURCES
    ${UCN_FOUNDATION_SOURCES}
    ${UCN_TRANSPORT_SOURCES}
    ${UCN_ADAPTER_SOURCES}
    ${UCN_NODE_SOURCES}
    ${UCN_ROUTING_SOURCES}
    ${UCN_SERVICE_SOURCES}
)

add_library(ucn_core STATIC ${UCN_CORE_SOURCES})

target_include_directories(ucn_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(UCN_USER_CONFIG_INCLUDE_DIR)
    if(NOT IS_DIRECTORY "${UCN_USER_CONFIG_INCLUDE_DIR}")
        message(FATAL_ERROR
            "UCN_USER_CONFIG_INCLUDE_DIR is not a directory: ${UCN_USER_CONFIG_INCLUDE_DIR}")
    endif()
    target_include_directories(ucn_core PUBLIC
        ${UCN_USER_CONFIG_INCLUDE_DIR}
    )
endif()

target_compile_definitions(ucn_core
    PUBLIC
        UCN_PROFILE=${UCN_PROFILE_VALUE}
        UCN_FEATURE_SERVICE=${UCN_FEATURE_SERVICE_VALUE}
)

if(UCN_USER_CONFIG_HEADER)
    target_compile_definitions(ucn_core PUBLIC
        "UCN_USER_CONFIG_HEADER=\"${UCN_USER_CONFIG_HEADER}\""
    )
    message(STATUS "UCN user config header: ${UCN_USER_CONFIG_HEADER}")
endif()

target_compile_options(ucn_core PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

# Large logical messages are an opt-in Extended component.  Keeping this as a
# separate archive means Core-only MCU products do not pay for reassembly RAM,
# retry state, or Transfer code unless they explicitly link ucn_transfer.
add_library(ucn_transfer STATIC EXCLUDE_FROM_ALL
    src/extended/ucn_transfer.c
)
target_link_libraries(ucn_transfer PUBLIC ucn_core)
target_compile_options(ucn_transfer PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

# CLV2-14-03: frozen wire codecs are physically independent archives.  A
# strict-v4 product links ucn_cluster_wire_v4 only; Current-FSM compatibility
# explicitly keeps UCN_CLUSTER_ENABLE_V3_COMPAT enabled.
if(UCN_CLUSTER_ENABLE_V3_COMPAT)
    add_library(ucn_cluster_wire_v3_compat STATIC EXCLUDE_FROM_ALL
        src/extended/cluster/ucn_cluster_codec_v3.c
    )
    target_link_libraries(ucn_cluster_wire_v3_compat PUBLIC ucn_core)
    target_compile_options(ucn_cluster_wire_v3_compat PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    )
endif()

add_library(ucn_cluster_wire_v4 STATIC EXCLUDE_FROM_ALL
    src/extended/cluster/ucn_cluster_codec_v4.c
)
target_link_libraries(ucn_cluster_wire_v4 PUBLIC ucn_core)
target_compile_options(ucn_cluster_wire_v4 PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

# The dual dispatcher is intentionally a third archive.  It cannot exist
# without the optional v3 decoder, and keeping it separate proves that the
# recommended strict-v4 archive has no hidden legacy symbol dependency.
if(UCN_CLUSTER_ENABLE_V3_COMPAT)
    add_library(ucn_cluster_wire_dual_stack STATIC EXCLUDE_FROM_ALL
        src/extended/cluster/ucn_cluster_wire_dispatch.c
    )
    target_link_libraries(ucn_cluster_wire_dual_stack PUBLIC
        ucn_cluster_wire_v3_compat
        ucn_cluster_wire_v4
    )
    target_compile_options(ucn_cluster_wire_dual_stack PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    )
endif()

# Automatic single-level clustering is also opt-in.  Election, membership and
# lease tables remain outside ucn_node_t so Core-only/Nano products retain the
# original footprint.
add_library(ucn_cluster STATIC EXCLUDE_FROM_ALL
    src/extended/ucn_cluster.c
    # CLV2-M02 (02-03): the explicit-phase transition framework.
    src/extended/cluster/ucn_cluster_fsm.c
    # CLV2-M02 (02-04): the member table + Join/Keepalive/Leave handlers.
    src/extended/cluster/ucn_cluster_membership.c
    # CLV2-M02 (02-05): the Backup selection/snapshot/heartbeat/resync and
    # Takeover prepare/ACK/complete lifecycle.
    src/extended/cluster/ucn_cluster_backup.c
    # CLV2-M09 (09-01): isolated committed/staging Backup mirror value model.
    src/extended/cluster/ucn_cluster_backup_mirror.c
    # CLV2-M09 (09-03): wire-agnostic strict Backup snapshot owner.
    src/extended/cluster/ucn_cluster_backup_sync.c
    # CLV2-M09 (09-10): pure strict Backup capability/profile gate.
    src/extended/cluster/ucn_cluster_backup_profile.c
    # CLV2-M02 (02-06): the RECOVERY_HEAD quorum/declaration/arbitration/TTL.
    src/extended/cluster/ucn_cluster_recovery.c
    # CLV2-M02 (02-06): the candidate table / head offer / stepdown / score.
    src/extended/cluster/ucn_cluster_merge.c
    # CLV2-M03 (03-01): the Epoch value type + pure comparator (infra).
    src/extended/cluster/ucn_cluster_epoch.c
    # CLV2-M04 (04-02): fixed, CRC-protected persistence record codec.
    src/extended/cluster/ucn_cluster_persist.c
    # CLV2-M04 (04-05): bounded submit/poll runtime and durable recheck.
    src/extended/cluster/ucn_cluster_persistence_runtime.c
    # CLV2-07-01: bounded C_old/C_joint/C_new value model only; no wire/FSM.
    src/extended/cluster/ucn_cluster_config_state.c
    src/extended/cluster/ucn_cluster_config_tx.c
    src/extended/cluster/ucn_cluster_config_proposal.c
    src/extended/cluster/ucn_cluster_config_quorum.c
    src/extended/cluster/ucn_cluster_config_store.c
    src/extended/cluster/ucn_cluster_config_persistence.c
    src/extended/cluster/ucn_cluster_config_backup.c
    src/extended/cluster/ucn_cluster_config_joint.c
    # CLV2-M08: opt-in controlled Authority/Quorum/Fence owner.  It has no
    # v4 wire RX/TX path and remains inert until explicitly attached.
    src/extended/cluster/ucn_cluster_authority.c
    # CLV2-14-04: read-only Safety-1..10 debug/fault invariant engine.
    src/extended/cluster/ucn_cluster_invariant.c
)
target_link_libraries(ucn_cluster PUBLIC ucn_core)
if(UCN_CLUSTER_ENABLE_V3_COMPAT)
    target_link_libraries(ucn_cluster PUBLIC ucn_cluster_wire_v3_compat)
else()
    message(STATUS
        "Cluster v3 compatibility: OFF; Current v3 FSM is intentionally not link-complete")
endif()
target_compile_options(ucn_cluster PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

# CLV2-M10: this model is deliberately opt-in and physically separate from
# the default Cluster archive. A product cannot pull in the frozen-majority
# takeover APIs merely by linking ucn_cluster; controlled experiments must
# explicitly link this target and still receive no v4 RX/TX/FSM/Authority
# integration from it.
if(UCN_BUILD_CLUSTER_TAKEOVER_EXPERIMENTAL)
    add_library(ucn_cluster_takeover_experimental STATIC EXCLUDE_FROM_ALL
        src/extended/cluster/ucn_cluster_takeover.c
        src/extended/cluster/ucn_cluster_takeover_persist.c
    )
    target_link_libraries(ucn_cluster_takeover_experimental
        PUBLIC ucn_cluster
    )
    target_compile_definitions(ucn_cluster_takeover_experimental PUBLIC
        UCN_CLUSTER_TAKEOVER_EXPERIMENTAL_ENABLED=1
    )
    target_compile_options(ucn_cluster_takeover_experimental PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    )
endif()

# CLV2-M11: the Head-to-Head Merge/Handover model is a second, independent
# opt-in archive.  It owns no production v4 codec, FSM, Authority or Adapter
# integration; an application must explicitly link it for controlled tests.
if(UCN_BUILD_CLUSTER_HANDOVER_EXPERIMENTAL)
    add_library(ucn_cluster_handover_experimental STATIC EXCLUDE_FROM_ALL
        src/extended/cluster/ucn_cluster_handover.c
    )
    target_link_libraries(ucn_cluster_handover_experimental PUBLIC ucn_cluster)
    target_compile_definitions(ucn_cluster_handover_experimental PUBLIC
        UCN_CLUSTER_HANDOVER_EXPERIMENTAL_ENABLED=1
    )
    target_compile_options(ucn_cluster_handover_experimental PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    )
endif()

# CLV2-M13: quorum Rekey and no-wrap continuation are isolated from the
# default Cluster archive.  The experimental owner consumes explicit M04/M08
# evidence but cannot parse production RX, send v4 frames or mutate the
# Current FSM merely because an application links ucn_cluster.
if(UCN_BUILD_CLUSTER_REKEY_EXPERIMENTAL)
    add_library(ucn_cluster_rekey_experimental STATIC EXCLUDE_FROM_ALL
        src/extended/cluster/ucn_cluster_rekey.c
        src/extended/cluster/ucn_cluster_rekey_persistence.c
        src/extended/cluster/ucn_cluster_id_history.c
    )
    target_link_libraries(ucn_cluster_rekey_experimental PUBLIC
        ucn_cluster
        ucn_cluster_wire_v4
    )
    target_compile_definitions(ucn_cluster_rekey_experimental PUBLIC
        UCN_CLUSTER_REKEY_EXPERIMENTAL_ENABLED=1
    )
    target_compile_options(ucn_cluster_rekey_experimental PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    )
endif()

# C06 federation format/state remains an opt-in Extended archive.  C06.1 only
# freezes the Endpoint codec and read-only Cluster boundary; no Core object
# or Wire v5 field changes are introduced here.
add_library(ucn_cluster_federation STATIC EXCLUDE_FROM_ALL
    src/extended/ucn_cluster_federation.c
)
target_link_libraries(ucn_cluster_federation PUBLIC ucn_cluster)
target_compile_options(ucn_cluster_federation PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

if(UCN_BUILD_CLUSTER_SIM)
    add_executable(ucn_cluster_sim tools/ucn_cluster_sim.c)
    # CLV2-07-12: the simulator must use the same production archive as a
    # product.  In particular it must not rebuild membership/RX with the
    # retired M06 legacy auto-commit bridge: v3 joins stay provisional and
    # v3 Backup/Takeover control remains fenced.
    target_link_libraries(ucn_cluster_sim PRIVATE ucn_cluster)
    target_compile_options(ucn_cluster_sim PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    )
endif()

# Platform Ports deliberately remain separate link targets.  A product links
# exactly one selected target (for example ucn_port_freertos); ucn_core itself
# contains no RTOS-specific wrapper or runtime hook.  EXCLUDE_FROM_ALL keeps
# unused Port code out of ordinary product builds.
function(ucn_add_platform_port target source)
    add_library(${target} STATIC EXCLUDE_FROM_ALL ${source})
    target_link_libraries(${target} PUBLIC ucn_core)
    target_compile_options(${target} PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    )
endfunction()

ucn_add_platform_port(ucn_port_bare_metal
    src/ports/ucn_port_bare_metal.c)
ucn_add_platform_port(ucn_port_freertos
    src/ports/ucn_port_freertos.c)
ucn_add_platform_port(ucn_port_zephyr
    src/ports/ucn_port_zephyr.c)
ucn_add_platform_port(ucn_port_nuttx
    src/ports/ucn_port_nuttx.c)
ucn_add_platform_port(ucn_port_rtthread
    src/ports/ucn_port_rtthread.c)
ucn_add_platform_port(ucn_port_host_fake
    src/ports/ucn_port_host_fake.c)

if(UCN_BUILD_SCALE_SIM AND NOT UCN_PROFILE_UPPER STREQUAL "NANO")
    add_executable(ucn_scale_sim tools/ucn_scale_sim.c)
    target_link_libraries(ucn_scale_sim PRIVATE ucn_core)
    target_compile_options(ucn_scale_sim PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    )
endif()

if(UCN_BUILD_TESTS)
    # CLV2-13-09: a source-level no-wrap contract is part of every test
    # profile. Products do not link Python or this script; it is a Host CI
    # gate that rejects direct mutation of Cluster safety serials.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME ucn_cluster_no_wrap_source_gate
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_cluster_no_wrap.py
            ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME ucn_cluster_phase_source_gate
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_cluster_phase_source.py
            ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME ucn_cluster_storage_boundary_gate
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_cluster_storage_boundary.py
            ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME ucn_cluster_wire_module_boundary_gate
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_cluster_wire_module_boundary.py
            ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME ucn_cluster_resource_contract_gate
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_cluster_resource_contract.py
            ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME ucn_cluster_docs_contract_gate
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_cluster_docs_contract.py
            ${CMAKE_CURRENT_SOURCE_DIR})

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
        tests/test_standard_adapter.c
        tests/test_link_cost.c
        tests/test_event_runtime.c
        tests/test_stream_source.c
        tests/test_can_source.c
        tests/test_protocol_owner.c
        tests/test_profile.c
        tests/test_public_headers.c
        tests/test_node_storage_header.c
        tests/test_cluster_storage_header.c
        tests/test_cluster_invariant.c
        tests/test_transfer.c
        tests/test_cluster.c
        tests/test_cluster_wire_v4_encoder_closed.c
        tests/test_cluster_persist.c
        tests/test_cluster_fuzz.c
        tests/test_cluster_federation.c
        tests/cluster_test_fixture.c
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

    # CLV2-01-04a: the test binary compiles its own copy of ucn_cluster.c
    # with the test-hook macro so the unit tests can reach the static
    # cluster_transition() entry point.  The executable's definitions take
    # precedence over the linked ucn_cluster archive, so the production
    # archive stays free of test hooks.  Static-library members are only
    # pulled for symbols the objects do not already define.
    target_sources(ucn_tests PRIVATE src/extended/ucn_cluster.c)
    # CLV2-M02 (02-02): the test binary also compiles its own codec copy so
    # the test's ucn_cluster.c links against the same codec definitions.
    target_sources(ucn_tests PRIVATE
        src/extended/cluster/ucn_cluster_codec_v3.c
    )
    # CLV2-M02 (02-03): ... and its own FSM copy (the test hooks live there
    # under UCN_CLUSTER_ENABLE_TEST_HOOKS).
    target_sources(ucn_tests PRIVATE
        src/extended/cluster/ucn_cluster_fsm.c
    )
    # CLV2-M02 (02-04): ... and its own membership copy.
    target_sources(ucn_tests PRIVATE
        src/extended/cluster/ucn_cluster_membership.c
    )
    # CLV2-M02 (02-05): ... and its own backup/takeover copy.
    target_sources(ucn_tests PRIVATE
        src/extended/cluster/ucn_cluster_backup.c
    )
    # CLV2-M02 (02-06): ... and its own recovery copy.
    target_sources(ucn_tests PRIVATE
        src/extended/cluster/ucn_cluster_recovery.c
    )
    # CLV2-M02 (02-06): ... and its own merge/head-offer copy.
    target_sources(ucn_tests PRIVATE
        src/extended/cluster/ucn_cluster_merge.c
    )
    # CLV2-M03 (03-01): ... and its own Epoch comparator copy.
    target_sources(ucn_tests PRIVATE
        src/extended/cluster/ucn_cluster_epoch.c
    )

    if(UCN_FEATURE_SERVICE)
        target_sources(ucn_tests PRIVATE
            tests/test_service.c
            tests/test_service_bridge.c
        )
    endif()

    # CLV2-M00.1: the golden REFERENCE lives in the source tree (read-only
    # in tests) while the ACTUAL trace is written to each build directory, so
    # parallel matrix routes never race on one shared file.
    target_compile_definitions(ucn_tests PRIVATE
        UCN_CLUSTER_GOLDEN_REFERENCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
        UCN_CLUSTER_GOLDEN_ACTUAL_DIR="${CMAKE_CURRENT_BINARY_DIR}"
        # CLV2-01-04a: expose the test-only cluster_transition() hooks.
        UCN_CLUSTER_ENABLE_TEST_HOOKS=1
    )
    # CLV2-06-09: only the test copy may include the internal membership
    # bridge contract.  Production targets keep this directory private to
    # their own source compilation and never receive the test-hook macro.
    target_include_directories(ucn_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/extended/cluster
    )

    target_link_libraries(ucn_tests PRIVATE
        ucn_core
        ucn_transfer
        ucn_cluster
        ucn_cluster_wire_v4
        ucn_cluster_federation
        ucn_port_bare_metal
        ucn_port_freertos
        ucn_port_zephyr
        ucn_port_nuttx
        ucn_port_rtthread
        ucn_port_host_fake
    )

    target_compile_options(ucn_tests PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    )

    add_test(NAME ucn_tests COMMAND ucn_tests)

    # RFC4 byte-level codec gate.  This executable compiles the isolated v4
    # codec with its test-only encoder switch enabled so its output can be
    # compared to frozen vectors.  The production ucn_cluster library keeps
    # UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED at its default zero value.
    add_executable(ucn_cluster_wire_v4_codec_tests
        tests/test_cluster_wire_v4_codec.c
        src/extended/cluster/ucn_cluster_codec_v3.c
        src/extended/cluster/ucn_cluster_codec_v4.c
        src/extended/cluster/ucn_cluster_wire_dispatch.c
    )
    # CLV2-05-03: semantic payload/builders are deliberately private to the
    # codec.  Only this focused codec test is allowed to include that header;
    # no public include path or production Cluster call site receives it.
    target_include_directories(ucn_cluster_wire_v4_codec_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/extended/cluster
    )
    target_link_libraries(ucn_cluster_wire_v4_codec_tests PRIVATE ucn_core)
    target_compile_definitions(ucn_cluster_wire_v4_codec_tests PRIVATE
        UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED=1
    )
    target_compile_options(ucn_cluster_wire_v4_codec_tests PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    )
    add_test(NAME ucn_cluster_wire_v4_codec_tests
        COMMAND ucn_cluster_wire_v4_codec_tests)

    # MAJOR-A: the RFC4 typed-offer helpers must retain byte-deterministic
    # outputs with optimisation enabled.  These are test-only copies of the
    # isolated codec; they neither enable the production encoder nor link the
    # production Cluster FSM.  The option is deliberately OFF by default so
    # product builds stay unaffected.
    if(UCN_ENABLE_WIRE_V4_RELEASE_GATES)
        function(ucn_add_wire_v4_optimized_codec_gate target_name)
            add_executable(${target_name}
                tests/test_cluster_wire_v4_codec.c
                src/extended/cluster/ucn_cluster_codec_v3.c
                src/extended/cluster/ucn_cluster_codec_v4.c
                src/extended/cluster/ucn_cluster_wire_dispatch.c
            )
            target_include_directories(${target_name} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src/extended/cluster
            )
            target_link_libraries(${target_name} PRIVATE ucn_core)
            target_compile_definitions(${target_name} PRIVATE
                UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED=1
                NDEBUG
            )
            target_compile_options(${target_name} PRIVATE
                $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
                ${ARGN}
            )
            add_test(NAME ${target_name} COMMAND ${target_name})
        endfunction()

        if(MSVC)
            ucn_add_wire_v4_optimized_codec_gate(
                ucn_cluster_wire_v4_codec_release_o1_tests /O1)
            ucn_add_wire_v4_optimized_codec_gate(
                ucn_cluster_wire_v4_codec_release_o2_tests /O2)
        else()
            ucn_add_wire_v4_optimized_codec_gate(
                ucn_cluster_wire_v4_codec_release_o1_tests -O1)
            ucn_add_wire_v4_optimized_codec_gate(
                ucn_cluster_wire_v4_codec_release_o2_tests -O2)
            ucn_add_wire_v4_optimized_codec_gate(
                ucn_cluster_wire_v4_codec_release_o3_tests -O3)
        endif()
    endif()

    # CLV2-05-12: a Host-only dual-stack compatibility gate.  It builds its
    # own isolated codecs with the test-only encoder enabled and deliberately
    # does not link ucn_cluster or call a Cluster runtime/FSM entry point.
    # The normal ucn_tests target separately proves that the production
    # default encoder remains fail-closed.
    add_executable(ucn_cluster_wire_v4_host_dual_stack_tests
        tests/test_cluster_wire_v4_host_dual_stack.c
        src/extended/cluster/ucn_cluster_codec_v3.c
        src/extended/cluster/ucn_cluster_codec_v4.c
        src/extended/cluster/ucn_cluster_wire_dispatch.c
    )
    target_link_libraries(ucn_cluster_wire_v4_host_dual_stack_tests PRIVATE ucn_core)
    target_compile_definitions(ucn_cluster_wire_v4_host_dual_stack_tests PRIVATE
        UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED=1
    )
    target_compile_options(ucn_cluster_wire_v4_host_dual_stack_tests PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    )
    add_test(NAME ucn_cluster_wire_v4_host_dual_stack_tests
        COMMAND ucn_cluster_wire_v4_host_dual_stack_tests)

    # CLV2-06-01: a data-model-only membership contract.  It links the
    # production Cluster archive to prove that legacy-v3 members remain
    # bounded, non-voting provisionals; it deliberately does not compile or
    # reference the v4 wire codec.
    add_executable(ucn_cluster_membership_model_tests
        tests/test_cluster_membership_model.c
    )
    target_include_directories(ucn_cluster_membership_model_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/extended/cluster
    )
    target_link_libraries(ucn_cluster_membership_model_tests PRIVATE ucn_cluster)
    target_compile_options(ucn_cluster_membership_model_tests PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    )
    add_test(NAME ucn_cluster_membership_model_tests
        COMMAND ucn_cluster_membership_model_tests)

    # CLV2-M09 (09-01): this target validates only the fixed-storage mirror
    # model.  It does not compile a v4 encoder or invoke Cluster RX/TX/FSM.
    add_executable(ucn_cluster_backup_mirror_tests
        tests/test_cluster_backup_mirror.c
    )
    target_link_libraries(ucn_cluster_backup_mirror_tests PRIVATE ucn_cluster)
    target_compile_options(ucn_cluster_backup_mirror_tests PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    )
    add_test(NAME ucn_cluster_backup_mirror_tests
        COMMAND ucn_cluster_backup_mirror_tests)

    # CLV2-M09 (09-03): source/Epoch/Config-gated SYNC_BEGIN only.  This
    # target never enables a v4 encoder or production Cluster RX/TX/FSM.
    add_executable(ucn_cluster_backup_sync_tests
        tests/test_cluster_backup_sync.c
    )
    target_link_libraries(ucn_cluster_backup_sync_tests PRIVATE ucn_cluster)
    target_compile_options(ucn_cluster_backup_sync_tests PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    )
    add_test(NAME ucn_cluster_backup_sync_tests
        COMMAND ucn_cluster_backup_sync_tests)

    # CLV2-M09 (09-10): v4-capability/profile selection only; no wire codec
    # or Cluster production RX/TX/FSM is linked through this test.
    add_executable(ucn_cluster_backup_profile_tests
        tests/test_cluster_backup_profile.c
    )
    target_link_libraries(ucn_cluster_backup_profile_tests PRIVATE ucn_cluster)
    target_compile_options(ucn_cluster_backup_profile_tests PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    )
    add_test(NAME ucn_cluster_backup_profile_tests
        COMMAND ucn_cluster_backup_profile_tests)

    if(UCN_BUILD_CLUSTER_TAKEOVER_EXPERIMENTAL)
        # CLV2-M10: exact frozen Config/Snapshot quorum and certificate model.
        # This target remains wire-agnostic and does not enable the v4 encoder.
        add_executable(ucn_cluster_takeover_tests
            tests/test_cluster_takeover.c
        )
        target_link_libraries(ucn_cluster_takeover_tests PRIVATE
            ucn_cluster_takeover_experimental
        )
        target_compile_options(ucn_cluster_takeover_tests PRIVATE
            $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
        )
        add_test(NAME ucn_cluster_takeover_tests
            COMMAND ucn_cluster_takeover_tests)

        # CLV2-14-05: executable target-property model. It combines the
        # actual M08 Authority owner, invariant engine and M10 frozen-quorum
        # transaction under exhaustive bounded partitions/subsets. It remains
        # default-OFF with M10 and never enables production v4 RX/TX/FSM.
        add_executable(ucn_cluster_target_property_tests
            tests/test_cluster_target_property.c
        )
        target_include_directories(ucn_cluster_target_property_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/extended/cluster
        )
        target_link_libraries(ucn_cluster_target_property_tests PRIVATE
            ucn_cluster_takeover_experimental
            ucn_cluster
        )
        target_compile_options(ucn_cluster_target_property_tests PRIVATE
            $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
        )
        add_test(NAME ucn_cluster_target_property_tests
            COMMAND ucn_cluster_target_property_tests)
    endif()

    if(UCN_BUILD_CLUSTER_HANDOVER_EXPERIMENTAL)
        add_executable(ucn_cluster_handover_tests
            tests/test_cluster_handover.c
        )
        target_link_libraries(ucn_cluster_handover_tests PRIVATE
            ucn_cluster_handover_experimental
        )
        target_compile_options(ucn_cluster_handover_tests PRIVATE
            $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
        )
        add_test(NAME ucn_cluster_handover_tests
            COMMAND ucn_cluster_handover_tests)
    endif()

    if(UCN_BUILD_CLUSTER_REKEY_EXPERIMENTAL)
        add_executable(ucn_cluster_rekey_tests
            tests/test_cluster_rekey.c
        )
        target_link_libraries(ucn_cluster_rekey_tests PRIVATE
            ucn_cluster_rekey_experimental
        )
        target_compile_options(ucn_cluster_rekey_tests PRIVATE
            $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
        )
        add_test(NAME ucn_cluster_rekey_tests COMMAND ucn_cluster_rekey_tests)
    endif()

    # CLV2-07-01: independent production-archive value-model gate. It has no
    # test bridge and no v4 encoder/dispatcher definition.
    add_executable(ucn_cluster_config_state_tests
        tests/test_cluster_config_state.c
    )
    target_link_libraries(ucn_cluster_config_state_tests PRIVATE ucn_cluster)
    target_compile_options(ucn_cluster_config_state_tests PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    )
    add_test(NAME ucn_cluster_config_state_tests
        COMMAND ucn_cluster_config_state_tests)

    add_executable(ucn_cluster_config_tx_tests
        tests/test_cluster_config_tx.c
    )
    target_link_libraries(ucn_cluster_config_tx_tests PRIVATE ucn_cluster)
    target_compile_options(ucn_cluster_config_tx_tests PRIVATE
        ${UCN_WARNING_OPTIONS}
    )
    add_test(NAME ucn_cluster_config_tx_tests
        COMMAND ucn_cluster_config_tx_tests)

    add_executable(ucn_cluster_config_proposal_tests
        tests/test_cluster_config_proposal.c
    )
    target_link_libraries(ucn_cluster_config_proposal_tests PRIVATE ucn_cluster)
    target_compile_options(ucn_cluster_config_proposal_tests PRIVATE
        ${UCN_WARNING_OPTIONS}
    )
    add_test(NAME ucn_cluster_config_proposal_tests
        COMMAND ucn_cluster_config_proposal_tests)

    add_executable(ucn_cluster_config_quorum_tests
        tests/test_cluster_config_quorum.c
    )
    target_link_libraries(ucn_cluster_config_quorum_tests PRIVATE ucn_cluster)
    target_compile_options(ucn_cluster_config_quorum_tests PRIVATE
        ${UCN_WARNING_OPTIONS}
    )
    add_test(NAME ucn_cluster_config_quorum_tests
        COMMAND ucn_cluster_config_quorum_tests)

    add_executable(ucn_cluster_config_persistence_tests
        tests/test_cluster_config_persistence.c
    )
    target_link_libraries(ucn_cluster_config_persistence_tests PRIVATE ucn_cluster)
    target_compile_options(ucn_cluster_config_persistence_tests PRIVATE
        ${UCN_WARNING_OPTIONS}
    )
    add_test(NAME ucn_cluster_config_persistence_tests
        COMMAND ucn_cluster_config_persistence_tests)

    add_executable(ucn_cluster_config_store_tests
        tests/test_cluster_config_store.c
    )
    target_link_libraries(ucn_cluster_config_store_tests PRIVATE ucn_cluster)
    target_compile_options(ucn_cluster_config_store_tests PRIVATE
        ${UCN_WARNING_OPTIONS}
    )
    add_test(NAME ucn_cluster_config_store_tests
        COMMAND ucn_cluster_config_store_tests)

    add_executable(ucn_cluster_config_backup_tests
        tests/test_cluster_config_backup.c
    )
    target_link_libraries(ucn_cluster_config_backup_tests PRIVATE ucn_cluster)
    target_compile_options(ucn_cluster_config_backup_tests PRIVATE
        ${UCN_WARNING_OPTIONS}
    )
    add_test(NAME ucn_cluster_config_backup_tests
        COMMAND ucn_cluster_config_backup_tests)

    add_executable(ucn_cluster_config_joint_tests
        tests/test_cluster_config_joint.c
    )
    target_link_libraries(ucn_cluster_config_joint_tests PRIVATE ucn_cluster)
    target_compile_options(ucn_cluster_config_joint_tests PRIVATE
        ${UCN_WARNING_OPTIONS}
    )
    add_test(NAME ucn_cluster_config_joint_tests
        COMMAND ucn_cluster_config_joint_tests)

    # CLV2-M08: controlled Authority Owner contract.  It links the normal
    # Cluster archive but provides no v4 encoder/RX/FSM target switch.
    add_executable(ucn_cluster_authority_tests
        tests/test_cluster_authority.c
    )
    target_link_libraries(ucn_cluster_authority_tests PRIVATE ucn_cluster)
    target_compile_options(ucn_cluster_authority_tests PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    )
    add_test(NAME ucn_cluster_authority_tests
        COMMAND ucn_cluster_authority_tests)

    if(TARGET ucn_cluster_sim)
        foreach(UCN_CLUSTER_SIM_NODES 64 256 1000)
            add_test(NAME ucn_cluster_${UCN_CLUSTER_SIM_NODES}_convergence
                COMMAND ucn_cluster_sim --nodes ${UCN_CLUSTER_SIM_NODES}
                    --scenario clean)
            set_tests_properties(ucn_cluster_${UCN_CLUSTER_SIM_NODES}_convergence
                PROPERTIES PASS_REGULAR_EXPRESSION
                    "CLUSTER_SIM nodes=${UCN_CLUSTER_SIM_NODES} scenario=clean.*converged_ms=[1-9]")

            add_test(NAME ucn_cluster_${UCN_CLUSTER_SIM_NODES}_impaired
                COMMAND ucn_cluster_sim --nodes ${UCN_CLUSTER_SIM_NODES}
                    --scenario impaired --seed 1592594996
                    --expect-m03-isolation)
            set_tests_properties(ucn_cluster_${UCN_CLUSTER_SIM_NODES}_impaired
                PROPERTIES PASS_REGULAR_EXPRESSION
                    "CLUSTER_SIM nodes=${UCN_CLUSTER_SIM_NODES} scenario=impaired.*dropped=[1-9].*delayed=[1-9].*duplicated=[1-9].*asymmetric=[1-9].*m03_isolation=passed")
        endforeach()

        # CLV2-14-07: run non-authority churn at every declared scale. The
        # head-failover case is an explicit negative product contract: v3
        # Backup/Takeover is fenced until production v4 Authority wiring, so
        # losing every Primary must fail closed rather than resurrect legacy
        # authority. It is not a successful failover claim.
        foreach(UCN_CLUSTER_SIM_NODES 64 256 1000)
            foreach(UCN_CLUSTER_SIM_SCENARIO mobility score-shift)
                add_test(NAME ucn_cluster_${UCN_CLUSTER_SIM_NODES}_${UCN_CLUSTER_SIM_SCENARIO}
                    COMMAND ucn_cluster_sim --nodes ${UCN_CLUSTER_SIM_NODES}
                        --scenario ${UCN_CLUSTER_SIM_SCENARIO})
                set_tests_properties(
                    ucn_cluster_${UCN_CLUSTER_SIM_NODES}_${UCN_CLUSTER_SIM_SCENARIO}
                    PROPERTIES PASS_REGULAR_EXPRESSION
                        "CLUSTER_SIM nodes=${UCN_CLUSTER_SIM_NODES} scenario=${UCN_CLUSTER_SIM_SCENARIO}.*converged_ms=[1-9]")
            endforeach()

            add_test(NAME ucn_cluster_${UCN_CLUSTER_SIM_NODES}_v3_authority_fence
                COMMAND ucn_cluster_sim --nodes ${UCN_CLUSTER_SIM_NODES}
                    --scenario head-failover --expect-v3-authority-fence)
            set_tests_properties(
                ucn_cluster_${UCN_CLUSTER_SIM_NODES}_v3_authority_fence
                PROPERTIES PASS_REGULAR_EXPRESSION
                    "CLUSTER_SIM nodes=${UCN_CLUSTER_SIM_NODES} scenario=head-failover.*v3_authority_fence=passed")
        endforeach()

        foreach(UCN_CLUSTER_SIM_GROUP 2 4 8)
            add_test(NAME ucn_cluster_64_fast_impaired_group_${UCN_CLUSTER_SIM_GROUP}
                COMMAND ucn_cluster_sim --nodes 64 --scenario impaired
                    --profile fast-fixed --seed 1592594996
                    --group ${UCN_CLUSTER_SIM_GROUP}
                    --expect-m03-isolation)
            set_tests_properties(
                ucn_cluster_64_fast_impaired_group_${UCN_CLUSTER_SIM_GROUP}
                PROPERTIES PASS_REGULAR_EXPRESSION
                    "CLUSTER_SIM nodes=64 scenario=impaired profile=fast-fixed.*dropped=[1-9].*delayed=[1-9].*duplicated=[1-9].*asymmetric=[1-9].*m03_isolation=passed")
        endforeach()
    endif()

    # This focused executable gives the preset resolver a direct product-header
    # entry point.  The complete ucn_tests target also supports the same
    # product-wide UCN_USER_CONFIG_HEADER and is the primary regression suite.
    if(UCN_USER_CONFIG_HEADER)
        add_executable(ucn_standard_adapter_product_config_test
            tests/test_standard_adapter_config_main.c
            tests/test_standard_adapter.c
        )
        target_link_libraries(ucn_standard_adapter_product_config_test PRIVATE ucn_core)
        target_compile_options(ucn_standard_adapter_product_config_test PRIVATE
            $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
        )
        add_test(NAME ucn_standard_adapter_product_config_test
            COMMAND ucn_standard_adapter_product_config_test)
    endif()

    if(UCN_BUILD_CONFIG_CONTRACT_TESTS)
        add_executable(ucn_config_defaults_test tests/test_config_contract.c)
        target_include_directories(ucn_config_defaults_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        )
        target_compile_options(ucn_config_defaults_test PRIVATE
            $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
        )
        add_test(NAME ucn_config_defaults_test COMMAND ucn_config_defaults_test)

        add_executable(ucn_config_fallback_test tests/test_config_contract.c)
        target_include_directories(ucn_config_fallback_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        )
        target_compile_definitions(ucn_config_fallback_test PRIVATE
            UCN_CONFIG_NO_DEFAULTS=1
        )
        target_compile_options(ucn_config_fallback_test PRIVATE
            $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
        )
        add_test(NAME ucn_config_fallback_test COMMAND ucn_config_fallback_test)

        add_executable(ucn_config_override_test tests/test_config_contract.c)
        target_include_directories(ucn_config_override_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/config
        )
        target_compile_definitions(ucn_config_override_test PRIVATE
            UCN_USER_CONFIG_HEADER="ucn_test_user_config.h"
            UCN_TEST_EXPECT_USER_OVERRIDES=1
        )
        target_compile_options(ucn_config_override_test PRIVATE
            $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
        )
        add_test(NAME ucn_config_override_test COMMAND ucn_config_override_test)
    endif()

    if(TARGET ucn_scale_sim)
        foreach(UCN_SCALE_WIRE_PROFILE w0 w1 w2 w3 mixed)
            add_test(NAME ucn_scale_${UCN_SCALE_WIRE_PROFILE}_smoke
                COMMAND ucn_scale_sim
                    --nodes 8
                    --ticks 20
                    --warmup-ticks 40
                    --drain-ticks 120
                    --topology auto
                    --traffic two-hop
                    --wire-profile ${UCN_SCALE_WIRE_PROFILE}
                    --quiet
            )
        endforeach()
        # The tree builder needs four Links at an internal node.  This
        # contract test explicitly reports SUPPORTED or CAPACITY_SKIPPED;
        # ordinary smoke tests use topology=auto and never claim a 3-Link
        # product executed the incompatible tree topology.
        add_test(NAME ucn_scale_tree_capacity_contract
            COMMAND ucn_scale_sim
                --nodes 8
                --topology tree
                --topology-capacity-contract
                --quiet
        )
        set_tests_properties(ucn_scale_tree_capacity_contract PROPERTIES
            PASS_REGULAR_EXPRESSION "SCALE_TOPOLOGY_CAPACITY_(SUPPORTED|SKIPPED)")
        add_test(NAME ucn_scale_w0_address_limit_reject
            COMMAND ucn_scale_sim
                --nodes 255
                --ticks 1
                --wire-profile w0
                --quiet
        )
        set_tests_properties(ucn_scale_w0_address_limit_reject
            PROPERTIES WILL_FAIL TRUE)
        add_test(NAME ucn_scale_mixed_address_limit_reject
            COMMAND ucn_scale_sim
                --nodes 255
                --ticks 1
                --wire-profile mixed
                --quiet
        )
        set_tests_properties(ucn_scale_mixed_address_limit_reject
            PROPERTIES WILL_FAIL TRUE)
        add_test(NAME ucn_scale_auto_smoke COMMAND ucn_scale_sim
            --nodes 8
            --ticks 20
            --warmup-ticks 40
            --drain-ticks 120
            --topology auto
            --traffic two-hop
            --wire-mode auto
            --wire-profile w3
            --quiet
        )
        add_test(NAME ucn_scale_end_to_end_smoke COMMAND ucn_scale_sim
            --nodes 8
            --topology line
            --traffic end-to-end
            --ticks 20
            --warmup-ticks 100
            --drain-ticks 200
            --wire-profile w3
            --quiet
        )
    endif()
endif()
