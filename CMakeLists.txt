cmake_minimum_required(VERSION 3.20)

project(UCN VERSION 6.0.0 LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_options($<$<C_COMPILER_ID:MSVC>:/utf-8>)

option(UCN_BUILD_TESTS "Build UCN v6 unit and validation tests" ON)
option(UCN_FEATURE_REALTIME "Build the optional v6 Realtime module" ON)
option(UCN_FEATURE_GROUP "Build the optional simplified Group module" ON)
option(UCN_FEATURE_CLUSTER "Build the optional v6 Cluster module" ON)
option(UCN_FEATURE_ADAPTER "Build v6 Adapter, bearer, RTOS, and reference ports" ON)
option(UCN_FEATURE_PERSISTENCE
    "Build the optional simplified Persistence Foundation" OFF)
option(UCN_VERIFY_IMPL02_SIGNED_CANDIDATE
    "Reproduce the historical signed IMPL-02 exact candidate (expected to fail after later-stage changes)"
    OFF)
set(UCN_PROFILE "FULL" CACHE STRING "UCN v6 resource profile: NANO, LITE, or FULL")
set_property(CACHE UCN_PROFILE PROPERTY STRINGS NANO LITE FULL)
set(UCN_USER_CONFIG_HEADER "" CACHE STRING
    "Optional product configuration header included before v6 defaults")
set(UCN_USER_CONFIG_INCLUDE_DIR "" CACHE PATH
    "Include directory containing UCN_USER_CONFIG_HEADER")
set(UCN_ASCII_STAGING_ROOT "" CACHE PATH
    "Explicit ASCII-only root for Windows package-consumer link gates")

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

foreach(feature REALTIME GROUP CLUSTER ADAPTER)
    if(UCN_FEATURE_${feature})
        set(UCN_FEATURE_${feature}_VALUE 1)
    else()
        set(UCN_FEATURE_${feature}_VALUE 0)
    endif()
endforeach()

message(STATUS
    "UCN v6 profile=${UCN_PROFILE_UPPER} realtime=${UCN_FEATURE_REALTIME} "
    "group=${UCN_FEATURE_GROUP} "
    "cluster=${UCN_FEATURE_CLUSTER} adapter=${UCN_FEATURE_ADAPTER} "
    "persistence=${UCN_FEATURE_PERSISTENCE}")

function(ucn_target_defaults target)
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    if(UCN_USER_CONFIG_INCLUDE_DIR)
        if(NOT IS_DIRECTORY "${UCN_USER_CONFIG_INCLUDE_DIR}")
            message(FATAL_ERROR
                "UCN_USER_CONFIG_INCLUDE_DIR is not a directory: ${UCN_USER_CONFIG_INCLUDE_DIR}")
        endif()
        target_include_directories(${target} PUBLIC
            $<BUILD_INTERFACE:${UCN_USER_CONFIG_INCLUDE_DIR}>)
    endif()
    target_compile_options(${target} PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Werror>
        $<$<C_COMPILER_ID:MSVC>:/W4;/WX>)
endfunction()

add_library(ucn_common STATIC
    src/core/ucn_checked.c
    src/core/ucn_digest.c
    src/core/ucn_owner.c)
add_library(UCN::common ALIAS ucn_common)
set_target_properties(ucn_common PROPERTIES EXPORT_NAME common)
ucn_target_defaults(ucn_common)
target_include_directories(ucn_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(ucn_coordinator STATIC src/core/ucn_coordinator.c)
ucn_target_defaults(ucn_coordinator)
target_include_directories(ucn_coordinator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ucn_coordinator PRIVATE ucn_common)

# IMPL-03..08 architecture scaffolds. These targets are deliberately private,
# excluded from product builds without tests, and absent from install/export.
# They freeze module ownership and Coordinator-routed dependency direction;
# they contain no public API, business state, Wire codec, or success path.
add_library(ucn_module_scaffold STATIC EXCLUDE_FROM_ALL
    src/core/ucn_module_scaffold.c)
ucn_target_defaults(ucn_module_scaffold)
target_include_directories(ucn_module_scaffold PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ucn_module_scaffold PRIVATE ucn_common)

function(ucn_add_v6s_module_scaffold target source)
    add_library(${target} STATIC EXCLUDE_FROM_ALL ${source})
    ucn_target_defaults(${target})
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(${target} PRIVATE
        ucn_common ucn_coordinator)
endfunction()

ucn_add_v6s_module_scaffold(ucn_v6s_security_scaffold
    src/security/ucn_security_scaffold.c)
ucn_add_v6s_module_scaffold(ucn_v6s_admission_scaffold
    src/admission/ucn_admission_scaffold.c)
ucn_add_v6s_module_scaffold(ucn_v6s_routing_scaffold
    src/routing/ucn_routing_scaffold.c)
ucn_add_v6s_module_scaffold(ucn_v6s_transport_scaffold
    src/transport/ucn_transport_scaffold.c)
ucn_add_v6s_module_scaffold(ucn_v6s_service_scaffold
    src/service/ucn_service_scaffold.c)
ucn_add_v6s_module_scaffold(ucn_v6s_realtime_scaffold
    src/realtime/ucn_realtime_scaffold.c)
ucn_add_v6s_module_scaffold(ucn_v6s_group_scaffold
    src/group/ucn_group_scaffold.c)
ucn_add_v6s_module_scaffold(ucn_v6s_cluster_scaffold
    src/cluster/ucn_cluster_scaffold.c)

# IMPL-03 detailed implementation is built only by its focused test while it
# is below the public/product connection boundary.  It owns fixed Session
# state and emits immutable Persistence requirements; it cannot call the
# Persistence Owner or enter the product archive yet.
add_library(ucn_v6s_security_session STATIC EXCLUDE_FROM_ALL
    src/security/ucn_security_session.c)
ucn_target_defaults(ucn_v6s_security_session)
target_include_directories(ucn_v6s_security_session PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(ucn_v6s_security_session PUBLIC
    UCN_PROFILE=${UCN_PROFILE_VALUE})
target_link_libraries(ucn_v6s_security_session PRIVATE ucn_common)

# IMPL-04 keeps Dynamic Admission and Capability as two independent private
# Owners. They exchange only immutable Coordinator-routed views and do not
# link Security or Persistence Owner archives directly.
add_library(ucn_v6s_admission STATIC EXCLUDE_FROM_ALL
    src/admission/ucn_admission.c)
ucn_target_defaults(ucn_v6s_admission)
target_include_directories(ucn_v6s_admission PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(ucn_v6s_admission PUBLIC
    UCN_PROFILE=${UCN_PROFILE_VALUE})
target_link_libraries(ucn_v6s_admission PRIVATE ucn_common)

add_library(ucn_v6s_identity_binding STATIC EXCLUDE_FROM_ALL
    src/admission/ucn_identity_binding.c)
ucn_target_defaults(ucn_v6s_identity_binding)
target_include_directories(ucn_v6s_identity_binding PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(ucn_v6s_identity_binding PUBLIC
    UCN_PROFILE=${UCN_PROFILE_VALUE})
target_link_libraries(ucn_v6s_identity_binding PRIVATE ucn_common)

add_library(ucn_v6s_capability STATIC EXCLUDE_FROM_ALL
    src/capability/ucn_capability.c)
ucn_target_defaults(ucn_v6s_capability)
target_include_directories(ucn_v6s_capability PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(ucn_v6s_capability PUBLIC
    UCN_PROFILE=${UCN_PROFILE_VALUE})
target_link_libraries(ucn_v6s_capability PRIVATE ucn_common)

# IMPL-05 base routing is a private, fixed-capacity Owner.  It consumes only
# Coordinator-provided immutable Link/identity facts and does not link the
# Security, Admission, Capability, Flow, or public Runtime archives.
add_library(ucn_v6s_route STATIC EXCLUDE_FROM_ALL
    src/routing/ucn_route.c)
ucn_target_defaults(ucn_v6s_route)
target_include_directories(ucn_v6s_route PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(ucn_v6s_route PUBLIC
    UCN_PROFILE=${UCN_PROFILE_VALUE})
target_link_libraries(ucn_v6s_route PRIVATE ucn_common)

# Advanced Flow stays a private typed model until the five Stage/Commit
# message bodies and their Golden/Negative vectors are externally frozen.
# Only the already frozen prefix and Label Setup codecs are implemented here.
add_library(ucn_v6s_flow STATIC EXCLUDE_FROM_ALL
    src/routing/ucn_flow.c)
ucn_target_defaults(ucn_v6s_flow)
target_include_directories(ucn_v6s_flow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(ucn_v6s_flow PUBLIC
    UCN_PROFILE=${UCN_PROFILE_VALUE})
target_link_libraries(ucn_v6s_flow PRIVATE ucn_common)

# IMPL-06 Transport owns C1 Reliable receipts/retry state, the frozen
# Setup/Fragment/SACK codecs, fixed-window Transfer state, and a typed durable
# C1 parent high-water consumer. It remains private and never links Route,
# Flow, Security, Persistence, Adapter, or the public Runtime directly.
add_library(ucn_v6s_transport STATIC EXCLUDE_FROM_ALL
    src/transport/ucn_transport.c
    src/transport/ucn_transport_codec.c
    src/transport/ucn_transfer.c
    src/transport/ucn_transport_parent.c)
ucn_target_defaults(ucn_v6s_transport)
target_include_directories(ucn_v6s_transport PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(ucn_v6s_transport PUBLIC
    UCN_PROFILE=${UCN_PROFILE_VALUE})
target_link_libraries(ucn_v6s_transport PRIVATE ucn_common)

# IMPL-07 starts as a private Service/QoS owner. It owns only volatile
# Request/Result correlation, retained duplicate receipts, and fixed queue
# references. Durable Operation and public Runtime wiring remain separate.
add_library(ucn_v6s_service STATIC EXCLUDE_FROM_ALL
    src/service/ucn_service.c
    src/service/ucn_qos.c
    src/service/ucn_operation.c
    src/service/ucn_operation_id.c)
ucn_target_defaults(ucn_v6s_service)
target_include_directories(ucn_v6s_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(ucn_v6s_service PUBLIC
    UCN_PROFILE=${UCN_PROFILE_VALUE})
target_link_libraries(ucn_v6s_service PRIVATE ucn_common)

# IMPL-08A Realtime is private and opt-in. Ordinary communication carries no
# time metadata; the owner consumes only immutable Coordinator-routed facts.
if(UCN_FEATURE_REALTIME)
    add_library(ucn_v6s_realtime STATIC EXCLUDE_FROM_ALL
        src/realtime/ucn_realtime.c
        src/realtime/ucn_realtime_codec.c
        src/realtime/ucn_realtime_domain.c
        src/realtime/ucn_realtime_sync.c
        src/realtime/ucn_realtime_policy.c)
    ucn_target_defaults(ucn_v6s_realtime)
    target_include_directories(ucn_v6s_realtime PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(ucn_v6s_realtime PUBLIC
        UCN_PROFILE=${UCN_PROFILE_VALUE})
    target_link_libraries(ucn_v6s_realtime PRIVATE ucn_common)
endif()

# IMPL-08B Group is an independent private Owner. Static Group needs no
# runtime Persistence; Dynamic Group emits only immutable typed requirements
# and consumes Coordinator-routed proof. It never links Cluster, Realtime,
# Security, Route, Adapter, or Persistence directly.
if(UCN_FEATURE_GROUP)
    add_library(ucn_v6s_group STATIC EXCLUDE_FROM_ALL
        src/group/ucn_group.c
        src/group/ucn_group_send.c
        src/group/ucn_group_receive.c
        src/group/ucn_group_step.c)
    ucn_target_defaults(ucn_v6s_group)
    target_include_directories(ucn_v6s_group PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(ucn_v6s_group PUBLIC
        UCN_PROFILE=${UCN_PROFILE_VALUE})
    target_link_libraries(ucn_v6s_group PRIVATE ucn_common)
endif()

# IMPL-08C Cluster is an independent private management-plane Owner. It
# consumes only Coordinator-routed immutable identity/security/Flow and
# Persistence facts. It never links Realtime, Group, Route, Security,
# Adapter, Coordinator, or Persistence directly.
if(UCN_FEATURE_CLUSTER)
    add_library(ucn_v6s_cluster STATIC EXCLUDE_FROM_ALL
        src/cluster/ucn_cluster.c
        src/cluster/ucn_cluster_config.c
        src/cluster/ucn_cluster_transition.c
        src/cluster/ucn_cluster_snapshot.c
        src/cluster/ucn_cluster_lineage.c
        src/cluster/ucn_cluster_directory.c
        src/cluster/ucn_cluster_persistence.c
        src/cluster/ucn_cluster_step.c)
    ucn_target_defaults(ucn_v6s_cluster)
    target_include_directories(ucn_v6s_cluster PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(ucn_v6s_cluster PUBLIC
        UCN_PROFILE=${UCN_PROFILE_VALUE})
    target_link_libraries(ucn_v6s_cluster PRIVATE ucn_common)
endif()

# IMPL-09-00 product composition planner.  It is a private, pure preflight:
# no Owner is initialized and no public/product target links the detailed
# modules yet.  The planner freezes capability-to-module closure, deterministic
# startup order, and fixed private-owner resource accounting before wiring
# real cross-Owner side effects.
add_library(ucn_v6s_composition STATIC EXCLUDE_FROM_ALL
    src/runtime/ucn_composition.c)
ucn_target_defaults(ucn_v6s_composition)
target_include_directories(ucn_v6s_composition PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(ucn_v6s_composition PUBLIC
    UCN_PROFILE=${UCN_PROFILE_VALUE}
    UCN_FEATURE_PERSISTENCE_ENABLED=$<BOOL:${UCN_FEATURE_PERSISTENCE}>
    UCN_V6S_FEATURE_ADAPTER_ENABLED=${UCN_FEATURE_ADAPTER_VALUE}
    UCN_V6S_FEATURE_REALTIME_ENABLED=${UCN_FEATURE_REALTIME_VALUE}
    UCN_V6S_FEATURE_GROUP_ENABLED=${UCN_FEATURE_GROUP_VALUE}
    UCN_V6S_FEATURE_CLUSTER_ENABLED=${UCN_FEATURE_CLUSTER_VALUE})
target_link_libraries(ucn_v6s_composition PRIVATE
    ucn_common
    ucn_coordinator
    ucn_kernel
    ucn_v6s_identity_binding
    ucn_v6s_security_session
    ucn_v6s_admission
    ucn_v6s_capability
    ucn_v6s_route
    ucn_v6s_flow
    ucn_v6s_transport
    ucn_v6s_service)
if(UCN_FEATURE_PERSISTENCE)
    target_link_libraries(ucn_v6s_composition PRIVATE ucn_persistence)
endif()
if(UCN_FEATURE_REALTIME)
    target_link_libraries(ucn_v6s_composition PRIVATE ucn_v6s_realtime)
endif()
if(UCN_FEATURE_GROUP)
    target_link_libraries(ucn_v6s_composition PRIVATE ucn_v6s_group)
endif()
if(UCN_FEATURE_CLUSTER)
    target_link_libraries(ucn_v6s_composition PRIVATE ucn_v6s_cluster)
endif()

set(UCN_V6S_SCAFFOLD_TARGETS
    ucn_module_scaffold
    ucn_v6s_security_scaffold
    ucn_v6s_admission_scaffold
    ucn_v6s_routing_scaffold
    ucn_v6s_transport_scaffold
    ucn_v6s_service_scaffold
    ucn_v6s_realtime_scaffold
    ucn_v6s_group_scaffold
    ucn_v6s_cluster_scaffold)

add_library(ucn_wire STATIC src/wire/ucn_wire.c)
ucn_target_defaults(ucn_wire)
target_include_directories(ucn_wire PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(ucn_wire PUBLIC UCN_PROFILE=${UCN_PROFILE_VALUE})
target_link_libraries(ucn_wire PRIVATE ucn_common)

add_library(ucn_adapter STATIC src/adapter/ucn_adapter.c)
ucn_target_defaults(ucn_adapter)
target_include_directories(ucn_adapter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(ucn_adapter PUBLIC UCN_PROFILE=${UCN_PROFILE_VALUE})
target_link_libraries(ucn_adapter PRIVATE ucn_common)

add_library(ucn_kernel STATIC src/runtime/ucn_node.c)
ucn_target_defaults(ucn_kernel)
target_include_directories(ucn_kernel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(ucn_kernel PUBLIC
    UCN_PROFILE=${UCN_PROFILE_VALUE}
    UCN_FEATURE_PERSISTENCE_ENABLED=$<BOOL:${UCN_FEATURE_PERSISTENCE}>)
target_link_libraries(ucn_kernel PUBLIC ucn_common PRIVATE ucn_wire ucn_adapter)

# Public simplified archive.  The module targets above remain independently
# testable and non-installed; users receive one archive and one link target.
add_library(ucn_simplified STATIC
    src/core/ucn_checked.c
    src/core/ucn_owner.c
    src/wire/ucn_wire.c
    src/adapter/ucn_adapter.c
    src/runtime/ucn_node.c)
add_library(UCN::simplified ALIAS ucn_simplified)
set_target_properties(ucn_simplified PROPERTIES EXPORT_NAME simplified)
ucn_target_defaults(ucn_simplified)
target_include_directories(ucn_simplified PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(ucn_simplified PUBLIC UCN_PROFILE=${UCN_PROFILE_VALUE})
if(UCN_FEATURE_PERSISTENCE)
    target_compile_definitions(ucn_simplified PUBLIC
        UCN_FEATURE_PERSISTENCE_ENABLED=1)
else()
    target_compile_definitions(ucn_simplified PUBLIC
        UCN_FEATURE_PERSISTENCE_ENABLED=0)
endif()

if(UCN_FEATURE_PERSISTENCE)
    add_library(ucn_persistence STATIC
        src/persistence/ucn_persistence_codec.c
        src/persistence/ucn_persistence.c)
    add_library(UCN::persistence ALIAS ucn_persistence)
    set_target_properties(ucn_persistence PROPERTIES EXPORT_NAME persistence)
    ucn_target_defaults(ucn_persistence)
    target_include_directories(ucn_persistence PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(ucn_persistence PUBLIC
        UCN_PROFILE=${UCN_PROFILE_VALUE}
        UCN_FEATURE_PERSISTENCE_ENABLED=1)
    add_library(ucn_persistence_coordinator STATIC
        src/persistence/ucn_persistence_coordinator.c)
    ucn_target_defaults(ucn_persistence_coordinator)
    target_include_directories(ucn_persistence_coordinator PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(ucn_persistence_coordinator PRIVATE
        UCN_PROFILE=${UCN_PROFILE_VALUE}
        UCN_FEATURE_PERSISTENCE_ENABLED=1)
    target_link_libraries(ucn_persistence_coordinator PRIVATE
        ucn_persistence ucn_coordinator)
    # UCN::simplified is the aggregate convenience target. Persistence keeps
    # its own archive/Owner boundary; its product-setup header must be included
    # explicitly and its Owner SPI remains private under src/internal/.
    target_link_libraries(ucn_simplified PUBLIC ucn_persistence)
endif()

add_library(ucn_v6_config STATIC src/v6/config/ucn_v6_config.c)
ucn_target_defaults(ucn_v6_config)
target_compile_definitions(ucn_v6_config PUBLIC
    UCN_V6_PROFILE=${UCN_PROFILE_VALUE}
    UCN_V6_FEATURE_REALTIME_ENABLED=${UCN_FEATURE_REALTIME_VALUE}
    UCN_V6_FEATURE_CLUSTER_ENABLED=${UCN_FEATURE_CLUSTER_VALUE}
    UCN_V6_FEATURE_ADAPTER_ENABLED=${UCN_FEATURE_ADAPTER_VALUE})
if(UCN_USER_CONFIG_HEADER)
    target_compile_definitions(ucn_v6_config PUBLIC
        "UCN_V6_USER_CONFIG_HEADER=\"${UCN_USER_CONFIG_HEADER}\"")
endif()

add_library(ucn_v6_identity STATIC
    src/v6/identity/ucn_v6_identity.c
    src/v6/identity/ucn_v6_bootstrap.c)
ucn_target_defaults(ucn_v6_identity)
target_link_libraries(ucn_v6_identity PUBLIC ucn_v6_config)

add_library(ucn_v6_wire STATIC src/v6/wire/ucn_v6_wire.c)
ucn_target_defaults(ucn_v6_wire)
target_link_libraries(ucn_v6_wire PUBLIC ucn_v6_identity)

add_library(ucn_v6_message STATIC src/v6/message/ucn_v6_message.c)
ucn_target_defaults(ucn_v6_message)
target_link_libraries(ucn_v6_message PUBLIC ucn_v6_wire)

add_library(ucn_v6_owner STATIC src/v6/owner/ucn_v6_owner.c)
ucn_target_defaults(ucn_v6_owner)
target_link_libraries(ucn_v6_owner PUBLIC ucn_v6_config)

add_library(ucn_v6_security STATIC src/v6/security/ucn_v6_security.c)
ucn_target_defaults(ucn_v6_security)
target_link_libraries(ucn_v6_security PUBLIC
    ucn_v6_config ucn_v6_identity ucn_v6_wire ucn_v6_owner)

add_library(ucn_v6_capability STATIC src/v6/capability/ucn_v6_capability.c)
ucn_target_defaults(ucn_v6_capability)
target_link_libraries(ucn_v6_capability PUBLIC
    ucn_v6_config ucn_v6_wire ucn_v6_security)

add_library(ucn_v6_route STATIC src/v6/route/ucn_v6_route.c)
ucn_target_defaults(ucn_v6_route)
target_link_libraries(ucn_v6_route PUBLIC
    ucn_v6_config ucn_v6_identity ucn_v6_capability)

add_library(ucn_v6_qos STATIC
    src/v6/qos/ucn_v6_metric.c
    src/v6/qos/ucn_v6_qos.c)
ucn_target_defaults(ucn_v6_qos)
target_link_libraries(ucn_v6_qos PUBLIC
    ucn_v6_config ucn_v6_message ucn_v6_security ucn_v6_route)

add_library(ucn_v6_transfer STATIC
    src/v6/transfer/ucn_v6_transfer_codec.c
    src/v6/transfer/ucn_v6_transfer.c)
ucn_target_defaults(ucn_v6_transfer)
target_link_libraries(ucn_v6_transfer PUBLIC
    ucn_v6_config ucn_v6_message ucn_v6_security ucn_v6_capability
    ucn_v6_route ucn_v6_qos)

set(UCN_EXPORT_TARGETS
    ucn_simplified
    ucn_v6_config ucn_v6_identity ucn_v6_wire ucn_v6_message ucn_v6_owner
    ucn_v6_security ucn_v6_capability ucn_v6_route ucn_v6_qos
    ucn_v6_transfer)
if(UCN_FEATURE_PERSISTENCE)
    list(APPEND UCN_EXPORT_TARGETS ucn_persistence)
endif()

if(UCN_FEATURE_REALTIME)
    add_library(ucn_v6_realtime STATIC src/v6/realtime/ucn_v6_realtime.c)
    ucn_target_defaults(ucn_v6_realtime)
    target_link_libraries(ucn_v6_realtime PUBLIC
        ucn_v6_config ucn_v6_wire ucn_v6_security ucn_v6_capability
        ucn_v6_route)
    list(APPEND UCN_EXPORT_TARGETS ucn_v6_realtime)
endif()

if(UCN_FEATURE_CLUSTER)
    add_library(ucn_v6_cluster STATIC src/v6/cluster/ucn_v6_cluster.c)
    ucn_target_defaults(ucn_v6_cluster)
    target_link_libraries(ucn_v6_cluster PUBLIC
        ucn_v6_config ucn_v6_wire ucn_v6_security ucn_v6_capability
        ucn_v6_route ucn_v6_transfer)
    list(APPEND UCN_EXPORT_TARGETS ucn_v6_cluster)
endif()

if(UCN_FEATURE_ADAPTER)
    add_library(ucn_v6_adapter STATIC
        src/v6/adapter/ucn_v6_adapter.c
        src/v6/adapter/ucn_v6_uart.c
        src/v6/adapter/ucn_v6_wifi.c
        src/v6/adapter/ucn_v6_can.c
        src/v6/adapter/ucn_v6_usb.c
        src/v6/ports/ucn_v6_freertos.c
        src/v6/reference/esp32s3/ucn_v6_esp32s3.c)
    ucn_target_defaults(ucn_v6_adapter)
    target_link_libraries(ucn_v6_adapter PUBLIC
        ucn_v6_config ucn_v6_wire ucn_v6_owner)
    list(APPEND UCN_EXPORT_TARGETS ucn_v6_adapter)

    add_library(ucn_v6_runtime STATIC
        src/v6/runtime/ucn_v6_runtime.c)
    ucn_target_defaults(ucn_v6_runtime)
    target_link_libraries(ucn_v6_runtime PUBLIC
        ucn_v6_adapter ucn_v6_identity ucn_v6_security
        ucn_v6_capability ucn_v6_route ucn_v6_qos ucn_v6_transfer)
    if(UCN_FEATURE_REALTIME)
        target_link_libraries(ucn_v6_runtime PUBLIC ucn_v6_realtime)
    endif()
    if(UCN_FEATURE_CLUSTER)
        target_link_libraries(ucn_v6_runtime PUBLIC ucn_v6_cluster)
    endif()
    list(APPEND UCN_EXPORT_TARGETS ucn_v6_runtime)
endif()

# GCC emits one deterministic .su record per compiled production source.  The
# gate is deliberately attached only to library targets: test fixtures may
# place many independent opaque-storage objects on their Host stack, while
# production entry points must stay within the MCU task-stack budget.
set(UCN_PRODUCTION_TARGETS
    ucn_common ucn_coordinator ucn_wire ucn_adapter ucn_kernel ucn_simplified
    ${UCN_EXPORT_TARGETS})
if(UCN_FEATURE_PERSISTENCE)
    list(APPEND UCN_PRODUCTION_TARGETS ucn_persistence_coordinator)
endif()
if(UCN_BUILD_TESTS AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    foreach(ucn_stack_target IN LISTS UCN_PRODUCTION_TARGETS)
        target_compile_options(${ucn_stack_target} PRIVATE -fstack-usage)
    endforeach()
    target_compile_options(ucn_common PRIVATE -fcallgraph-info=su)
    target_compile_options(ucn_coordinator PRIVATE -fcallgraph-info=su)
    target_compile_options(ucn_wire PRIVATE -fcallgraph-info=su)
    target_compile_options(ucn_adapter PRIVATE -fcallgraph-info=su)
    target_compile_options(ucn_kernel PRIVATE -fcallgraph-info=su)
    target_compile_options(ucn_simplified PRIVATE -fcallgraph-info=su)
    if(UCN_FEATURE_PERSISTENCE)
        target_compile_options(ucn_persistence PRIVATE -fcallgraph-info=su)
        target_compile_options(ucn_persistence_coordinator PRIVATE
            -fcallgraph-info=su)
    endif()
    foreach(ucn_scaffold_target IN LISTS UCN_V6S_SCAFFOLD_TARGETS)
        target_compile_options(${ucn_scaffold_target} PRIVATE -fstack-usage)
    endforeach()
    target_compile_options(ucn_v6s_security_session PRIVATE -fstack-usage)
    target_compile_options(ucn_v6s_admission PRIVATE -fstack-usage)
    target_compile_options(ucn_v6s_identity_binding PRIVATE -fstack-usage)
    target_compile_options(ucn_v6s_capability PRIVATE -fstack-usage)
    target_compile_options(ucn_v6s_route PRIVATE -fstack-usage)
    target_compile_options(ucn_v6s_flow PRIVATE -fstack-usage)
    target_compile_options(ucn_v6s_transport PRIVATE -fstack-usage)
    target_compile_options(ucn_v6s_service PRIVATE -fstack-usage)
    if(TARGET ucn_v6s_realtime)
        target_compile_options(ucn_v6s_realtime PRIVATE
            -fstack-usage -fcallgraph-info=su)
    endif()
    if(TARGET ucn_v6s_group)
        target_compile_options(ucn_v6s_group PRIVATE
            -fstack-usage -fcallgraph-info=su)
    endif()
    if(TARGET ucn_v6s_cluster)
        target_compile_options(ucn_v6s_cluster PRIVATE
            -fstack-usage -fcallgraph-info=su)
    endif()
    target_compile_options(ucn_v6s_composition PRIVATE -fstack-usage)
endif()

add_library(ucn INTERFACE)
add_library(UCN::ucn ALIAS ucn)
target_link_libraries(ucn INTERFACE
    ucn_v6_config ucn_v6_identity ucn_v6_wire ucn_v6_message ucn_v6_owner
    ucn_v6_security ucn_v6_capability ucn_v6_route ucn_v6_qos
    ucn_v6_transfer)
if(UCN_FEATURE_REALTIME)
    target_link_libraries(ucn INTERFACE ucn_v6_realtime)
endif()
if(UCN_FEATURE_CLUSTER)
    target_link_libraries(ucn INTERFACE ucn_v6_cluster)
endif()
if(UCN_FEATURE_ADAPTER)
    target_link_libraries(ucn INTERFACE ucn_v6_adapter ucn_v6_runtime)
endif()
list(APPEND UCN_EXPORT_TARGETS ucn)

install(TARGETS ${UCN_EXPORT_TARGETS}
    EXPORT UCNTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/ucn/ucn.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ucn)
install(FILES
    include/ucn/ucn_simplified.h
    include/ucn/ucn_types.h
    include/ucn/ucn_config.h
    include/ucn/ucn_driver.h
    include/ucn/ucn_product.h
    include/ucn/ucn_core.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ucn)
if(UCN_FEATURE_PERSISTENCE)
    install(FILES include/ucn/ucn_persistence.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ucn)
endif()
set(UCN_V6_BASE_PUBLIC_HEADERS
    include/ucn/v6/ucn_v6_bootstrap.h
    include/ucn/v6/ucn_v6_capability.h
    include/ucn/v6/ucn_v6_config.h
    include/ucn/v6/ucn_v6_identity.h
    include/ucn/v6/ucn_v6_message.h
    include/ucn/v6/ucn_v6_owner.h
    include/ucn/v6/ucn_v6_qos.h
    include/ucn/v6/ucn_v6_route.h
    include/ucn/v6/ucn_v6_security.h
    include/ucn/v6/ucn_v6_transfer.h
    include/ucn/v6/ucn_v6_types.h
    include/ucn/v6/ucn_v6_wire.h)
install(FILES ${UCN_V6_BASE_PUBLIC_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ucn/v6)
if(UCN_FEATURE_REALTIME)
    install(FILES include/ucn/v6/ucn_v6_realtime.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ucn/v6)
endif()
if(UCN_FEATURE_CLUSTER)
    install(FILES include/ucn/v6/ucn_v6_cluster.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ucn/v6)
endif()
if(UCN_FEATURE_ADAPTER)
    install(FILES
        include/ucn/v6/ucn_v6_adapter.h
        include/ucn/v6/ucn_v6_runtime.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ucn/v6)
    install(DIRECTORY
        include/ucn/v6/adapters
        include/ucn/v6/ports
        include/ucn/v6/reference
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ucn/v6)
endif()
install(EXPORT UCNTargets
    FILE UCNTargets.cmake
    NAMESPACE UCN::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/UCN)
configure_package_config_file(
    cmake/UCNConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/UCNConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/UCN)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/UCNConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/UCNConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/UCNConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/UCN)

if(UCN_BUILD_TESTS)
    enable_testing()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    if(CMAKE_NM)
        set(UCN_SYMBOL_TOOL "${CMAKE_NM}")
    else()
        find_program(UCN_SYMBOL_TOOL NAMES dumpbin llvm-nm nm REQUIRED)
    endif()

    function(ucn_add_test name source library)
        add_executable(${name} ${source})
        ucn_target_defaults(${name})
        target_link_libraries(${name} PRIVATE ${library})
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    add_test(NAME v6_source_boundary_gate
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_boundaries.py
            --root ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME v6s_impl_boundary_gate
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6s_impl_boundaries.py
            --root ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME v6_archive_symbol_gate
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_archives.py
            --build-dir ${CMAKE_CURRENT_BINARY_DIR}
            --symbol-tool ${UCN_SYMBOL_TOOL})
    add_test(NAME v6_current_docs_gate
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_current_docs.py
            --root ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME v6s_contract_freeze_gate
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6s_freeze_contracts.py
            --root ${CMAKE_CURRENT_SOURCE_DIR})
    if(UCN_FEATURE_PERSISTENCE AND UCN_VERIFY_IMPL02_SIGNED_CANDIDATE)
        add_test(NAME v6s_impl02_candidate_gate
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/generate_v6s_impl02_manifest.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
                --check)
    endif()
    add_test(NAME v6_api_index_gate
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/generate_v6_api_index.py
            --root ${CMAKE_CURRENT_SOURCE_DIR}
            --output ${CMAKE_CURRENT_SOURCE_DIR}/docs/源码阅读指南/06-公共函数签名索引.md
            --check)
    add_test(NAME v6_init_overlap_guard_gate
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_init_overlap_guards.py
            --root ${CMAKE_CURRENT_SOURCE_DIR})
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        add_test(NAME v6_stack_usage_gate
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_stack_usage.py
                --build-dir ${CMAKE_CURRENT_BINARY_DIR}
                --limit-bytes 3072)
        add_test(NAME v6s_common_stack_usage_gate
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_stack_usage.py
                --build-dir ${CMAKE_CURRENT_BINARY_DIR}
                --limit-bytes 256
                --source-prefix /src/core/)
        add_test(NAME v6s_common_call_stack_gate
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_call_stack.py
                --build-dir ${CMAKE_CURRENT_BINARY_DIR}
                --limit-bytes 768
                --source-prefix /src/core/)
        add_test(NAME v6s_kernel_stack_usage_gate
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_stack_usage.py
                --build-dir ${CMAKE_CURRENT_BINARY_DIR}
                --limit-bytes 256
                --source-prefix /src/wire/
                --source-prefix /src/adapter/
                --source-prefix /src/runtime/
                --source-prefix /src/persistence/)
        if(UCN_PROFILE_UPPER STREQUAL "NANO")
            set(UCN_SIMPLIFIED_CALL_STACK_LIMIT 768)
        elseif(UCN_PROFILE_UPPER STREQUAL "LITE")
            set(UCN_SIMPLIFIED_CALL_STACK_LIMIT 1536)
        else()
            set(UCN_SIMPLIFIED_CALL_STACK_LIMIT 3072)
        endif()
        add_test(NAME v6s_kernel_call_stack_gate
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_call_stack.py
                --build-dir ${CMAKE_CURRENT_BINARY_DIR}
                --limit-bytes ${UCN_SIMPLIFIED_CALL_STACK_LIMIT}
                --source-prefix /src/core/
                --source-prefix /src/wire/
                --source-prefix /src/adapter/
                --source-prefix /src/runtime/
                --source-prefix /src/persistence/)
        if(NOT CMAKE_C_FLAGS MATCHES "-fsanitize=")
            add_test(NAME v6s_security_stack_usage_gate
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_stack_usage.py
                    --build-dir ${CMAKE_CURRENT_BINARY_DIR}
                    --limit-bytes 256
                    --source-prefix /src/security/)
            add_test(NAME v6s_admission_stack_usage_gate
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_stack_usage.py
                    --build-dir ${CMAKE_CURRENT_BINARY_DIR}
                    --limit-bytes 256
                    --source-prefix /src/admission/
                    --source-prefix /src/capability/)
            add_test(NAME v6s_routing_stack_usage_gate
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_stack_usage.py
                    --build-dir ${CMAKE_CURRENT_BINARY_DIR}
                    --limit-bytes 1024
                    --source-prefix /src/routing/)
            add_test(NAME v6s_transport_stack_usage_gate
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_stack_usage.py
                    --build-dir ${CMAKE_CURRENT_BINARY_DIR}
                    --limit-bytes 1024
                    --source-prefix /src/transport/)
            add_test(NAME v6s_service_stack_usage_gate
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_stack_usage.py
                    --build-dir ${CMAKE_CURRENT_BINARY_DIR}
                    --limit-bytes 1024
                    --source-prefix /src/service/)
            if(TARGET ucn_v6s_realtime)
                add_test(NAME v6s_realtime_stack_usage_gate
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_stack_usage.py
                        --build-dir ${CMAKE_CURRENT_BINARY_DIR}
                        --limit-bytes 384
                        --source-prefix /src/realtime/)
                add_test(NAME v6s_realtime_call_stack_gate
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_call_stack.py
                        --build-dir ${CMAKE_CURRENT_BINARY_DIR}
                        --limit-bytes ${UCN_SIMPLIFIED_CALL_STACK_LIMIT}
                        --source-prefix /src/realtime/)
            endif()
            if(TARGET ucn_v6s_group)
                add_test(NAME v6s_group_stack_usage_gate
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_stack_usage.py
                        --build-dir ${CMAKE_CURRENT_BINARY_DIR}
                        --limit-bytes 384
                        --source-prefix /src/group/)
                add_test(NAME v6s_group_call_stack_gate
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_call_stack.py
                        --build-dir ${CMAKE_CURRENT_BINARY_DIR}
                        --limit-bytes ${UCN_SIMPLIFIED_CALL_STACK_LIMIT}
                        --source-prefix /src/group/)
            endif()
            if(TARGET ucn_v6s_cluster)
                add_test(NAME v6s_cluster_stack_usage_gate
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_stack_usage.py
                        --build-dir ${CMAKE_CURRENT_BINARY_DIR}
                        --limit-bytes 384
                        --source-prefix /src/cluster/)
                add_test(NAME v6s_cluster_call_stack_gate
                    COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_call_stack.py
                        --build-dir ${CMAKE_CURRENT_BINARY_DIR}
                        --limit-bytes ${UCN_SIMPLIFIED_CALL_STACK_LIMIT}
                        --source-prefix /src/cluster/)
            endif()
        endif()
    endif()
    add_test(NAME v6_removed_cmake_surface_gate
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_removed_cmake_surface.py
            --cmake ${CMAKE_COMMAND}
            --source ${CMAKE_CURRENT_SOURCE_DIR}
            --work ${CMAKE_CURRENT_BINARY_DIR}/v6-removed-cmake-gate
            --generator ${CMAKE_GENERATOR})

    ucn_add_test(ucn_v6_config_contract_tests
        tests/v6/test_v6_config_contract.c ucn_v6_config)
    ucn_add_test(ucn_common_tests tests/simplified/test_common.c ucn_common)
    target_include_directories(ucn_common_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    ucn_add_test(ucn_manifest_contract_tests
        tests/simplified/test_manifest_contract.c ucn_simplified)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        ucn_add_test(ucn_common_short_enum_tests
            tests/simplified/test_common.c ucn_common)
        target_include_directories(ucn_common_short_enum_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_options(ucn_common_short_enum_tests PRIVATE
            -fshort-enums)
    endif()
    ucn_add_test(ucn_owner_foundation_tests
        tests/simplified/test_owner_foundation.c ucn_common)
    target_include_directories(ucn_owner_foundation_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(UNIX)
        find_package(Threads REQUIRED)
        ucn_add_test(ucn_owner_concurrency_tests
            tests/simplified/test_owner_concurrency.c ucn_common)
        target_include_directories(ucn_owner_concurrency_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(ucn_owner_concurrency_tests PRIVATE
            Threads::Threads)
        ucn_add_test(ucn_coordinator_concurrency_tests
            tests/simplified/test_coordinator_concurrency.c ucn_coordinator)
        target_include_directories(ucn_coordinator_concurrency_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(ucn_coordinator_concurrency_tests PRIVATE
            Threads::Threads)
        ucn_add_test(ucn_core_concurrency_tests
            tests/simplified/test_core_concurrency.c ucn_kernel ucn_wire)
        target_include_directories(ucn_core_concurrency_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(ucn_core_concurrency_tests PRIVATE
            Threads::Threads)
        ucn_add_test(ucn_core_concurrency_driver_gate_tests
            tests/simplified/test_driver_gate_concurrency.c ucn_adapter)
        target_include_directories(ucn_core_concurrency_driver_gate_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(ucn_core_concurrency_driver_gate_tests PRIVATE
            Threads::Threads)
    endif()
    ucn_add_test(ucn_coordinator_tests
        tests/simplified/test_coordinator.c ucn_coordinator)
    target_include_directories(ucn_coordinator_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_executable(ucn_v6s_module_scaffold_tests
        tests/simplified/test_module_scaffolds.c)
    ucn_target_defaults(ucn_v6s_module_scaffold_tests)
    target_include_directories(ucn_v6s_module_scaffold_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(ucn_v6s_module_scaffold_tests PRIVATE
        ucn_module_scaffold
        ucn_v6s_security_scaffold
        ucn_v6s_admission_scaffold
        ucn_v6s_routing_scaffold
        ucn_v6s_transport_scaffold
        ucn_v6s_service_scaffold
        ucn_v6s_realtime_scaffold
        ucn_v6s_group_scaffold
        ucn_v6s_cluster_scaffold)
    add_test(NAME ucn_v6s_module_scaffold_tests
        COMMAND ucn_v6s_module_scaffold_tests)
    ucn_add_test(ucn_v6s_security_session_tests
        tests/simplified/test_security_session.c
        ucn_v6s_security_session)
    target_include_directories(ucn_v6s_security_session_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    ucn_add_test(ucn_v6s_security_resource_report
        tests/simplified/test_security_resources.c
        ucn_v6s_security_session)
    target_include_directories(ucn_v6s_security_resource_report PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(UNIX)
        add_executable(ucn_v6s_security_concurrency_tests
            tests/simplified/test_security_concurrency.c)
        ucn_target_defaults(ucn_v6s_security_concurrency_tests)
        target_include_directories(ucn_v6s_security_concurrency_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_definitions(ucn_v6s_security_concurrency_tests PRIVATE
            UCN_PROFILE=${UCN_PROFILE_VALUE})
        target_link_libraries(ucn_v6s_security_concurrency_tests PRIVATE
            ucn_v6s_security_session pthread)
        add_test(NAME ucn_v6s_security_concurrency_tests
            COMMAND ucn_v6s_security_concurrency_tests)
    endif()
    ucn_add_test(ucn_v6s_admission_tests
        tests/simplified/test_admission.c
        ucn_v6s_admission)
    target_include_directories(ucn_v6s_admission_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR})
    if(UNIX)
        add_executable(ucn_v6s_admission_concurrency_tests
            tests/simplified/test_admission_concurrency.c)
        ucn_target_defaults(ucn_v6s_admission_concurrency_tests)
        target_include_directories(
            ucn_v6s_admission_concurrency_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_definitions(
            ucn_v6s_admission_concurrency_tests PRIVATE
            UCN_PROFILE=${UCN_PROFILE_VALUE})
        target_link_libraries(
            ucn_v6s_admission_concurrency_tests PRIVATE
            ucn_v6s_admission Threads::Threads)
        add_test(NAME ucn_v6s_admission_concurrency_tests
            COMMAND ucn_v6s_admission_concurrency_tests)
    endif()
    ucn_add_test(ucn_v6s_identity_binding_tests
        tests/simplified/test_identity_binding.c
        ucn_v6s_identity_binding)
    target_include_directories(ucn_v6s_identity_binding_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    ucn_add_test(ucn_v6s_capability_tests
        tests/simplified/test_capability.c
        ucn_v6s_capability)
    target_include_directories(ucn_v6s_capability_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR})
    ucn_add_test(ucn_v6s_route_tests
        tests/simplified/test_route.c
        ucn_v6s_route)
    target_include_directories(ucn_v6s_route_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR})
    ucn_add_test(ucn_v6s_flow_tests
        tests/simplified/test_flow.c
        ucn_v6s_flow)
    target_include_directories(ucn_v6s_flow_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR})
    ucn_add_test(ucn_v6s_transport_reliable_tests
        tests/simplified/test_transport_reliable.c
        ucn_v6s_transport)
    target_include_directories(ucn_v6s_transport_reliable_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    ucn_add_test(ucn_v6s_transfer_tests
        tests/simplified/test_transfer.c
        ucn_v6s_transport)
    target_include_directories(ucn_v6s_transfer_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    ucn_add_test(ucn_v6s_transport_parent_tests
        tests/simplified/test_transport_parent.c
        ucn_v6s_transport)
    target_include_directories(ucn_v6s_transport_parent_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    ucn_add_test(ucn_v6s_service_tests
        tests/simplified/test_service.c
        ucn_v6s_service)
    target_include_directories(ucn_v6s_service_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    ucn_add_test(ucn_v6s_qos_tests
        tests/simplified/test_qos.c
        ucn_v6s_service)
    target_include_directories(ucn_v6s_qos_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    ucn_add_test(ucn_v6s_operation_tests
        tests/simplified/test_operation.c
        ucn_v6s_service)
    target_include_directories(ucn_v6s_operation_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_executable(ucn_v6s_service_resource_report
        tests/simplified/test_service_resources.c)
    ucn_target_defaults(ucn_v6s_service_resource_report)
    target_include_directories(ucn_v6s_service_resource_report PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(ucn_v6s_service_resource_report PRIVATE
        UCN_PROFILE=${UCN_PROFILE_VALUE})
    target_link_libraries(ucn_v6s_service_resource_report PRIVATE
        ucn_v6s_service)
    add_test(NAME ucn_v6s_service_resource_report
        COMMAND ucn_v6s_service_resource_report)
    ucn_add_test(ucn_v6s_composition_tests
        tests/simplified/test_composition.c
        ucn_v6s_composition)
    target_include_directories(ucn_v6s_composition_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    ucn_add_test(ucn_v6s_composition_storage_tests
        tests/simplified/test_composition_storage.c
        ucn_v6s_composition)
    target_include_directories(ucn_v6s_composition_storage_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(UCN_FEATURE_ADAPTER)
        ucn_add_test(ucn_v6s_composition_lifecycle_tests
            tests/simplified/test_composition_lifecycle.c
            ucn_v6s_composition)
        target_include_directories(
            ucn_v6s_composition_lifecycle_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
    endif()
    if(UCN_FEATURE_REALTIME)
      ucn_add_test(ucn_v6s_realtime_codec_tests
        tests/simplified/test_realtime_codec.c
        ucn_v6s_realtime)
    target_include_directories(ucn_v6s_realtime_codec_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    ucn_add_test(ucn_v6s_realtime_domain_tests
        tests/simplified/test_realtime_domain.c
        ucn_v6s_realtime)
    target_include_directories(ucn_v6s_realtime_domain_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    ucn_add_test(ucn_v6s_realtime_sync_tests
        tests/simplified/test_realtime_sync.c
        ucn_v6s_realtime)
    target_include_directories(ucn_v6s_realtime_sync_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    ucn_add_test(ucn_v6s_realtime_policy_tests
        tests/simplified/test_realtime_policy.c
        ucn_v6s_realtime)
    target_include_directories(ucn_v6s_realtime_policy_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_executable(ucn_v6s_realtime_resource_report
        tests/simplified/test_realtime_resources.c)
    ucn_target_defaults(ucn_v6s_realtime_resource_report)
    target_include_directories(ucn_v6s_realtime_resource_report PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(ucn_v6s_realtime_resource_report PRIVATE
        UCN_PROFILE=${UCN_PROFILE_VALUE})
    target_link_libraries(ucn_v6s_realtime_resource_report PRIVATE
        ucn_v6s_realtime)
    add_test(NAME ucn_v6s_realtime_resource_report
        COMMAND ucn_v6s_realtime_resource_report)
    if(UNIX)
        add_executable(ucn_v6s_realtime_concurrency_tests
            tests/simplified/test_realtime_concurrency.c)
        ucn_target_defaults(ucn_v6s_realtime_concurrency_tests)
        target_include_directories(ucn_v6s_realtime_concurrency_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_definitions(ucn_v6s_realtime_concurrency_tests PRIVATE
            UCN_PROFILE=${UCN_PROFILE_VALUE})
        target_link_libraries(ucn_v6s_realtime_concurrency_tests PRIVATE
            ucn_v6s_realtime Threads::Threads)
        add_test(NAME ucn_v6s_realtime_concurrency_tests
            COMMAND ucn_v6s_realtime_concurrency_tests)
    endif()
    endif()
    if(UCN_FEATURE_GROUP)
      ucn_add_test(ucn_v6s_group_context_tests
        tests/simplified/test_group_context.c
        ucn_v6s_group)
      target_include_directories(ucn_v6s_group_context_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
      ucn_add_test(ucn_v6s_group_send_tests
        tests/simplified/test_group_send.c
        ucn_v6s_group)
      target_include_directories(ucn_v6s_group_send_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
      ucn_add_test(ucn_v6s_group_receive_tests
        tests/simplified/test_group_receive.c
        ucn_v6s_group)
      target_include_directories(ucn_v6s_group_receive_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
      add_executable(ucn_v6s_group_resource_report
        tests/simplified/test_group_resources.c)
      ucn_target_defaults(ucn_v6s_group_resource_report)
      target_include_directories(ucn_v6s_group_resource_report PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
      target_compile_definitions(ucn_v6s_group_resource_report PRIVATE
        UCN_PROFILE=${UCN_PROFILE_VALUE})
      target_link_libraries(ucn_v6s_group_resource_report PRIVATE
        ucn_v6s_group)
      add_test(NAME ucn_v6s_group_resource_report
        COMMAND ucn_v6s_group_resource_report)
      if(UNIX)
        add_executable(ucn_v6s_group_concurrency_tests
          tests/simplified/test_group_concurrency.c)
        ucn_target_defaults(ucn_v6s_group_concurrency_tests)
        target_include_directories(ucn_v6s_group_concurrency_tests PRIVATE
          ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_definitions(ucn_v6s_group_concurrency_tests PRIVATE
          UCN_PROFILE=${UCN_PROFILE_VALUE})
        target_link_libraries(ucn_v6s_group_concurrency_tests PRIVATE
          ucn_v6s_group Threads::Threads)
        add_test(NAME ucn_v6s_group_concurrency_tests
          COMMAND ucn_v6s_group_concurrency_tests)
      endif()
    endif()
    if(UCN_FEATURE_CLUSTER)
      ucn_add_test(ucn_v6s_cluster_context_tests
        tests/simplified/test_cluster_context.c
        ucn_v6s_cluster)
      target_include_directories(ucn_v6s_cluster_context_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
      ucn_add_test(ucn_v6s_cluster_transition_tests
        tests/simplified/test_cluster_transition.c
        ucn_v6s_cluster)
      target_include_directories(ucn_v6s_cluster_transition_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
      ucn_add_test(ucn_v6s_cluster_snapshot_tests
        tests/simplified/test_cluster_snapshot.c
        ucn_v6s_cluster)
      target_include_directories(ucn_v6s_cluster_snapshot_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
      ucn_add_test(ucn_v6s_cluster_lineage_tests
        tests/simplified/test_cluster_lineage.c
        ucn_v6s_cluster)
      target_include_directories(ucn_v6s_cluster_lineage_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
      ucn_add_test(ucn_v6s_cluster_directory_tests
        tests/simplified/test_cluster_directory.c
        ucn_v6s_cluster)
      target_include_directories(ucn_v6s_cluster_directory_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
      add_executable(ucn_v6s_cluster_resource_report
        tests/simplified/test_cluster_resources.c)
      ucn_target_defaults(ucn_v6s_cluster_resource_report)
      target_include_directories(ucn_v6s_cluster_resource_report PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
      target_compile_definitions(ucn_v6s_cluster_resource_report PRIVATE
        UCN_PROFILE=${UCN_PROFILE_VALUE})
      target_link_libraries(ucn_v6s_cluster_resource_report PRIVATE
        ucn_v6s_cluster)
      add_test(NAME ucn_v6s_cluster_resource_report
        COMMAND ucn_v6s_cluster_resource_report)
      if(UNIX)
        add_executable(ucn_v6s_cluster_concurrency_tests
          tests/simplified/test_cluster_concurrency.c)
        ucn_target_defaults(ucn_v6s_cluster_concurrency_tests)
        target_include_directories(ucn_v6s_cluster_concurrency_tests PRIVATE
          ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_definitions(ucn_v6s_cluster_concurrency_tests PRIVATE
          UCN_PROFILE=${UCN_PROFILE_VALUE})
        target_link_libraries(ucn_v6s_cluster_concurrency_tests PRIVATE
          ucn_v6s_cluster Threads::Threads)
        add_test(NAME ucn_v6s_cluster_concurrency_tests
          COMMAND ucn_v6s_cluster_concurrency_tests)
      endif()
    endif()
    if(UNIX)
        add_executable(ucn_v6s_service_concurrency_tests
            tests/simplified/test_service_concurrency.c)
        ucn_target_defaults(ucn_v6s_service_concurrency_tests)
        target_include_directories(ucn_v6s_service_concurrency_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_definitions(ucn_v6s_service_concurrency_tests PRIVATE
            UCN_PROFILE=${UCN_PROFILE_VALUE})
        target_link_libraries(ucn_v6s_service_concurrency_tests PRIVATE
            ucn_v6s_service Threads::Threads)
        add_test(NAME ucn_v6s_service_concurrency_tests
            COMMAND ucn_v6s_service_concurrency_tests)
    endif()
    add_executable(ucn_v6s_transport_resource_report
        tests/simplified/test_transport_resources.c)
    ucn_target_defaults(ucn_v6s_transport_resource_report)
    target_include_directories(ucn_v6s_transport_resource_report PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(ucn_v6s_transport_resource_report PRIVATE
        UCN_PROFILE=${UCN_PROFILE_VALUE})
    target_link_libraries(ucn_v6s_transport_resource_report PRIVATE
        ucn_v6s_transport)
    add_test(NAME ucn_v6s_transport_resource_report
        COMMAND ucn_v6s_transport_resource_report)
    if(UNIX)
        add_executable(ucn_v6s_transport_concurrency_tests
            tests/simplified/test_transport_concurrency.c)
        ucn_target_defaults(ucn_v6s_transport_concurrency_tests)
        target_include_directories(ucn_v6s_transport_concurrency_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_definitions(ucn_v6s_transport_concurrency_tests PRIVATE
            UCN_PROFILE=${UCN_PROFILE_VALUE})
        target_link_libraries(ucn_v6s_transport_concurrency_tests PRIVATE
            ucn_v6s_transport Threads::Threads)
        add_test(NAME ucn_v6s_transport_concurrency_tests
            COMMAND ucn_v6s_transport_concurrency_tests)
    endif()
    add_executable(ucn_v6s_routing_resource_report
        tests/simplified/test_routing_resources.c)
    ucn_target_defaults(ucn_v6s_routing_resource_report)
    target_include_directories(ucn_v6s_routing_resource_report PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(ucn_v6s_routing_resource_report PRIVATE
        UCN_PROFILE=${UCN_PROFILE_VALUE})
    target_link_libraries(ucn_v6s_routing_resource_report PRIVATE
        ucn_v6s_route ucn_v6s_flow)
    add_test(NAME ucn_v6s_routing_resource_report
        COMMAND ucn_v6s_routing_resource_report)
    if(UNIX)
        add_executable(ucn_v6s_routing_concurrency_tests
            tests/simplified/test_routing_concurrency.c)
        ucn_target_defaults(ucn_v6s_routing_concurrency_tests)
        target_include_directories(ucn_v6s_routing_concurrency_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_definitions(ucn_v6s_routing_concurrency_tests PRIVATE
            UCN_PROFILE=${UCN_PROFILE_VALUE})
        target_link_libraries(ucn_v6s_routing_concurrency_tests PRIVATE
            ucn_v6s_route ucn_v6s_flow Threads::Threads)
        add_test(NAME ucn_v6s_routing_concurrency_tests
            COMMAND ucn_v6s_routing_concurrency_tests)
    endif()
    add_executable(ucn_v6s_admission_resource_report
        tests/simplified/test_admission_resources.c)
    ucn_target_defaults(ucn_v6s_admission_resource_report)
    target_include_directories(ucn_v6s_admission_resource_report PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(ucn_v6s_admission_resource_report PRIVATE
        UCN_PROFILE=${UCN_PROFILE_VALUE})
    add_test(NAME ucn_v6s_admission_resource_report
        COMMAND ucn_v6s_admission_resource_report)
    ucn_add_test(ucn_common_resource_report
        tests/simplified/test_common_resources.c ucn_coordinator)
    target_include_directories(ucn_common_resource_report PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    ucn_add_test(ucn_wire_c1_tests
        tests/simplified/test_wire_c1.c ucn_wire)
    target_include_directories(ucn_wire_c1_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    ucn_add_test(ucn_adapter_foundation_tests
        tests/simplified/test_adapter.c ucn_adapter)
    target_include_directories(ucn_adapter_foundation_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    ucn_add_test(ucn_core_static_tests
        tests/simplified/test_core_static.c ucn_kernel)
    target_include_directories(ucn_core_static_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    ucn_add_test(ucn_core_resource_report
        tests/simplified/test_core_resources.c ucn_kernel)
    target_include_directories(ucn_core_resource_report PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(UCN_FEATURE_PERSISTENCE)
        add_library(v6s_persistence_test_support STATIC
            tests/simplified/fake_persistence_provider.c)
        ucn_target_defaults(v6s_persistence_test_support)
        target_include_directories(v6s_persistence_test_support PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/simplified)
        target_compile_definitions(v6s_persistence_test_support PUBLIC
            UCN_PROFILE=${UCN_PROFILE_VALUE}
            UCN_FEATURE_PERSISTENCE_ENABLED=1)
        if(TARGET ucn_v6s_composition_lifecycle_tests)
            target_include_directories(
                ucn_v6s_composition_lifecycle_tests PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/simplified)
            target_link_libraries(
                ucn_v6s_composition_lifecycle_tests PRIVATE
                v6s_persistence_test_support)
        endif()

        ucn_add_test(ucn_persistence_codec_tests
            tests/simplified/test_persistence_codec.c ucn_persistence)
        target_include_directories(ucn_persistence_codec_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/rust/tests/conformance)

        add_executable(ucn_persistence_foundation_tests
            tests/simplified/test_persistence.c)
        ucn_target_defaults(ucn_persistence_foundation_tests)
        target_include_directories(ucn_persistence_foundation_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/simplified
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_definitions(ucn_persistence_foundation_tests PRIVATE
            UCN_PROFILE=${UCN_PROFILE_VALUE}
            UCN_FEATURE_PERSISTENCE_ENABLED=1)
        target_link_libraries(ucn_persistence_foundation_tests PRIVATE
            ucn_persistence v6s_persistence_test_support)
        add_test(NAME ucn_persistence_foundation_tests
            COMMAND ucn_persistence_foundation_tests)

        add_executable(ucn_persistence_coordinator_tests
            tests/simplified/test_persistence_coordinator.c)
        ucn_target_defaults(ucn_persistence_coordinator_tests)
        target_include_directories(ucn_persistence_coordinator_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/simplified
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_definitions(ucn_persistence_coordinator_tests PRIVATE
            UCN_PROFILE=${UCN_PROFILE_VALUE}
            UCN_FEATURE_PERSISTENCE_ENABLED=1)
        target_link_libraries(ucn_persistence_coordinator_tests PRIVATE
            ucn_persistence_coordinator v6s_persistence_test_support)
        add_test(NAME ucn_persistence_coordinator_tests
            COMMAND ucn_persistence_coordinator_tests)

        ucn_add_test(ucn_persistence_resource_report
            tests/simplified/test_persistence_resources.c ucn_persistence)
        target_include_directories(ucn_persistence_resource_report PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)

        add_executable(ucn_v6s_identity_persistence_integration_tests
            tests/simplified/test_identity_persistence_integration.c)
        ucn_target_defaults(ucn_v6s_identity_persistence_integration_tests)
        target_include_directories(
            ucn_v6s_identity_persistence_integration_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/simplified
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_definitions(
            ucn_v6s_identity_persistence_integration_tests PRIVATE
            UCN_PROFILE=${UCN_PROFILE_VALUE}
            UCN_FEATURE_PERSISTENCE_ENABLED=1)
        target_link_libraries(
            ucn_v6s_identity_persistence_integration_tests PRIVATE
            ucn_v6s_identity_binding ucn_persistence
            v6s_persistence_test_support)
        add_test(NAME ucn_v6s_identity_persistence_integration_tests
            COMMAND ucn_v6s_identity_persistence_integration_tests)

        add_executable(ucn_v6s_transport_persistence_integration_tests
            tests/simplified/test_transport_persistence_integration.c)
        ucn_target_defaults(ucn_v6s_transport_persistence_integration_tests)
        target_include_directories(
            ucn_v6s_transport_persistence_integration_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/simplified
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_definitions(
            ucn_v6s_transport_persistence_integration_tests PRIVATE
            UCN_PROFILE=${UCN_PROFILE_VALUE}
            UCN_FEATURE_PERSISTENCE_ENABLED=1)
        target_link_libraries(
            ucn_v6s_transport_persistence_integration_tests PRIVATE
            ucn_v6s_transport ucn_persistence
            v6s_persistence_test_support)
        add_test(NAME ucn_v6s_transport_persistence_integration_tests
            COMMAND ucn_v6s_transport_persistence_integration_tests)

        add_executable(ucn_v6s_service_persistence_integration_tests
            tests/simplified/test_service_persistence_integration.c)
        ucn_target_defaults(ucn_v6s_service_persistence_integration_tests)
        target_include_directories(
            ucn_v6s_service_persistence_integration_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/simplified
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_definitions(
            ucn_v6s_service_persistence_integration_tests PRIVATE
            UCN_PROFILE=${UCN_PROFILE_VALUE}
            UCN_FEATURE_PERSISTENCE_ENABLED=1)
        target_link_libraries(
            ucn_v6s_service_persistence_integration_tests PRIVATE
            ucn_v6s_service ucn_persistence
            v6s_persistence_test_support)
        add_test(NAME ucn_v6s_service_persistence_integration_tests
            COMMAND ucn_v6s_service_persistence_integration_tests)

        if(UCN_FEATURE_REALTIME)
          add_executable(ucn_v6s_realtime_persistence_integration_tests
            tests/simplified/test_realtime_persistence_integration.c)
        ucn_target_defaults(ucn_v6s_realtime_persistence_integration_tests)
        target_include_directories(
            ucn_v6s_realtime_persistence_integration_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/simplified
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_compile_definitions(
            ucn_v6s_realtime_persistence_integration_tests PRIVATE
            UCN_PROFILE=${UCN_PROFILE_VALUE}
            UCN_FEATURE_PERSISTENCE_ENABLED=1)
        target_link_libraries(
            ucn_v6s_realtime_persistence_integration_tests PRIVATE
            ucn_v6s_realtime ucn_persistence
            v6s_persistence_test_support)
        add_test(NAME ucn_v6s_realtime_persistence_integration_tests
            COMMAND ucn_v6s_realtime_persistence_integration_tests)
        endif()

        if(UCN_FEATURE_GROUP)
          add_executable(ucn_v6s_group_persistence_integration_tests
            tests/simplified/test_group_persistence_integration.c)
          ucn_target_defaults(ucn_v6s_group_persistence_integration_tests)
          target_include_directories(
            ucn_v6s_group_persistence_integration_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/simplified
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
          target_compile_definitions(
            ucn_v6s_group_persistence_integration_tests PRIVATE
            UCN_PROFILE=${UCN_PROFILE_VALUE}
            UCN_FEATURE_PERSISTENCE_ENABLED=1)
          target_link_libraries(
            ucn_v6s_group_persistence_integration_tests PRIVATE
            ucn_v6s_group ucn_persistence
            v6s_persistence_test_support)
          add_test(NAME ucn_v6s_group_persistence_integration_tests
            COMMAND ucn_v6s_group_persistence_integration_tests)
        endif()

        if(UCN_FEATURE_CLUSTER)
          add_executable(ucn_v6s_cluster_persistence_integration_tests
            tests/simplified/test_cluster_persistence_integration.c)
          ucn_target_defaults(ucn_v6s_cluster_persistence_integration_tests)
          target_include_directories(
            ucn_v6s_cluster_persistence_integration_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/simplified
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
          target_compile_definitions(
            ucn_v6s_cluster_persistence_integration_tests PRIVATE
            UCN_PROFILE=${UCN_PROFILE_VALUE}
            UCN_FEATURE_PERSISTENCE_ENABLED=1)
          target_link_libraries(
            ucn_v6s_cluster_persistence_integration_tests PRIVATE
            ucn_v6s_cluster ucn_persistence
            v6s_persistence_test_support)
          add_test(NAME ucn_v6s_cluster_persistence_integration_tests
            COMMAND ucn_v6s_cluster_persistence_integration_tests)
        endif()

        if(UNIX)
            add_executable(ucn_persistence_concurrency_tests
                tests/simplified/test_persistence_concurrency.c)
            ucn_target_defaults(ucn_persistence_concurrency_tests)
            target_include_directories(ucn_persistence_concurrency_tests PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/simplified
                ${CMAKE_CURRENT_SOURCE_DIR}/src)
            target_compile_definitions(ucn_persistence_concurrency_tests PRIVATE
                UCN_PROFILE=${UCN_PROFILE_VALUE}
                UCN_FEATURE_PERSISTENCE_ENABLED=1)
            target_link_libraries(ucn_persistence_concurrency_tests PRIVATE
                ucn_persistence v6s_persistence_test_support Threads::Threads)
            add_test(NAME ucn_persistence_concurrency_tests
                COMMAND ucn_persistence_concurrency_tests)
        endif()
    endif()
    ucn_add_test(ucn_v6_config_mismatch_tests
        tests/v6/test_v6_config_contract.c ucn_v6_config)
    target_compile_definitions(ucn_v6_config_mismatch_tests PRIVATE
        UCN_V6_CONFIG_MAX_BINDINGS=15
        UCN_V6_EXPECT_MANIFEST_MISMATCH=1)
    ucn_add_test(ucn_v6_identity_tests tests/v6/test_v6_identity.c
        ucn_v6_identity)
    ucn_add_test(ucn_v6_wire_tests tests/v6/test_v6_wire.c ucn_v6_wire)
    ucn_add_test(ucn_v6_message_tests tests/v6/test_v6_message.c
        ucn_v6_message)
    ucn_add_test(ucn_v6_owner_tests tests/v6/test_v6_owner.c ucn_v6_owner)
    ucn_add_test(ucn_v6_security_tests tests/v6/test_v6_security.c ucn)
    # The security executable also composes Capability -> Route -> QoS ->
    # optional Adapter -> relay -> destination Security -> Transfer. Keep a
    # separately named gate so release evidence cannot hide that pipeline
    # behind a module-local test label.
    add_test(NAME v6_secure_multihop_pipeline
        COMMAND ucn_v6_security_tests)
    ucn_add_test(ucn_v6_capability_tests tests/v6/test_v6_capability.c
        ucn_v6_capability)
    ucn_add_test(ucn_v6_route_tests tests/v6/test_v6_route.c ucn_v6_route)
    ucn_add_test(ucn_v6_qos_tests tests/v6/test_v6_qos.c ucn_v6_qos)
    ucn_add_test(ucn_v6_transfer_tests tests/v6/test_v6_transfer.c
        ucn_v6_transfer)
    ucn_add_test(ucn_v6_public_umbrella_tests
        tests/v6/test_v6_public_umbrella.c ucn)

    if(UCN_FEATURE_REALTIME)
        ucn_add_test(ucn_v6_realtime_tests tests/v6/test_v6_realtime.c
            ucn_v6_realtime)
    endif()
    if(UCN_FEATURE_CLUSTER)
        ucn_add_test(ucn_v6_cluster_tests tests/v6/test_v6_cluster.c
            ucn_v6_cluster)
        add_executable(ucn_v6_scale_sim tools/v6/ucn_v6_scale_sim.c)
        ucn_target_defaults(ucn_v6_scale_sim)
        target_link_libraries(ucn_v6_scale_sim PRIVATE ucn_v6_cluster)
        add_test(NAME v6_scale_1000 COMMAND ucn_v6_scale_sim 1000)
        add_test(NAME v6_scale_10000 COMMAND ucn_v6_scale_sim 10000)
    endif()
    if(UCN_FEATURE_ADAPTER)
        ucn_add_test(ucn_v6_adapter_tests tests/v6/test_v6_adapter.c
            ucn_v6_adapter)
        ucn_add_test(ucn_v6_runtime_tests tests/v6/test_v6_runtime.c
            ucn_v6_runtime)
        if(UNIX)
            find_package(Threads REQUIRED)
            ucn_add_test(ucn_v6_adapter_concurrency_tests
                tests/v6/test_v6_adapter_concurrency.c ucn_v6_adapter)
            target_link_libraries(ucn_v6_adapter_concurrency_tests PRIVATE
                Threads::Threads)
        endif()
    endif()

    add_executable(ucn_v6_resource_report tools/v6/ucn_v6_resource_report.c)
    ucn_target_defaults(ucn_v6_resource_report)
    target_link_libraries(ucn_v6_resource_report PRIVATE ucn_v6_config)
    add_test(NAME v6_resource_report COMMAND ucn_v6_resource_report)

    add_test(NAME v6_evidence_validator_selftest
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/test_validate_v6_evidence.py
            --validator
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/validate_v6_evidence.py)

    # MinGW's GNU ld cannot open absolute archive paths containing non-ASCII
    # characters even though CMake/Ninja can build and install them.  The
    # staging root is therefore an explicit, validated contract.  TEMP is only
    # a convenience default when its complete normalized path is ASCII; a
    # Unicode TEMP fails at configure time with an actionable override instead
    # of failing later in the package-consumer linker.
    if(WIN32)
        if(UCN_ASCII_STAGING_ROOT)
            file(TO_CMAKE_PATH "${UCN_ASCII_STAGING_ROOT}"
                UCN_VALIDATED_ASCII_STAGING_ROOT)
        elseif(DEFINED ENV{TEMP} AND NOT "$ENV{TEMP}" STREQUAL "")
            file(TO_CMAKE_PATH "$ENV{TEMP}"
                UCN_VALIDATED_ASCII_STAGING_ROOT)
        else()
            message(FATAL_ERROR
                "Windows package-consumer gates require an ASCII staging root. "
                "Set -DUCN_ASCII_STAGING_ROOT=C:/ucn-stage")
        endif()
        if(NOT IS_ABSOLUTE "${UCN_VALIDATED_ASCII_STAGING_ROOT}")
            message(FATAL_ERROR
                "UCN_ASCII_STAGING_ROOT must resolve to an absolute path: "
                "${UCN_VALIDATED_ASCII_STAGING_ROOT}")
        endif()
        string(REGEX MATCH "^[ -~]+$" UCN_ASCII_STAGING_MATCH
            "${UCN_VALIDATED_ASCII_STAGING_ROOT}")
        if(NOT UCN_ASCII_STAGING_MATCH STREQUAL
                "${UCN_VALIDATED_ASCII_STAGING_ROOT}")
            message(FATAL_ERROR
                "Windows package-consumer staging path is not ASCII: "
                "${UCN_VALIDATED_ASCII_STAGING_ROOT}. Set an explicit "
                "-DUCN_ASCII_STAGING_ROOT=C:/ucn-stage")
        endif()
        string(SHA256 UCN_CONSUMER_WORK_HASH "${CMAKE_CURRENT_BINARY_DIR}")
        string(SUBSTRING "${UCN_CONSUMER_WORK_HASH}" 0 16 UCN_CONSUMER_WORK_HASH)
        set(UCN_CONSUMER_WORK_DIR
            "${UCN_VALIDATED_ASCII_STAGING_ROOT}/ucn-v6-consumer-${UCN_CONSUMER_WORK_HASH}")
        set(UCN_SIMPLIFIED_CONSUMER_WORK_DIR
            "${UCN_VALIDATED_ASCII_STAGING_ROOT}/ucn-v6s-consumer-${UCN_CONSUMER_WORK_HASH}")
    else()
        set(UCN_CONSUMER_WORK_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/v6-install-consumer")
        set(UCN_SIMPLIFIED_CONSUMER_WORK_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/v6s-install-consumer")
    endif()

    # A single-config install consumer must use the same compiler family as
    # the producer build.  Otherwise a Clang parent build could silently run
    # the ABI gate with the first GCC/G++ found on PATH.
    set(UCN_SIMPLIFIED_CONSUMER_COMPILER_ARGS)
    if(NOT MSVC)
        get_filename_component(UCN_TEST_COMPILER_DIR
            "${CMAKE_C_COMPILER}" DIRECTORY)
        if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
            find_program(UCN_TEST_CXX_COMPILER
                NAMES c++.exe g++.exe c++ g++
                HINTS "${UCN_TEST_COMPILER_DIR}" NO_DEFAULT_PATH)
        elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
            find_program(UCN_TEST_CXX_COMPILER
                NAMES clang++.exe clang++
                HINTS "${UCN_TEST_COMPILER_DIR}" NO_DEFAULT_PATH)
        endif()
        if(NOT UCN_TEST_CXX_COMPILER)
            message(FATAL_ERROR
                "a matching C++ compiler is required for the simplified ABI consumer")
        endif()
        list(APPEND UCN_SIMPLIFIED_CONSUMER_COMPILER_ARGS
            "-DUCN_TEST_C_COMPILER=${CMAKE_C_COMPILER}"
            "-DUCN_TEST_CXX_COMPILER=${UCN_TEST_CXX_COMPILER}")
        if(DEFINED CMAKE_RC_COMPILER AND NOT CMAKE_RC_COMPILER STREQUAL "")
            list(APPEND UCN_SIMPLIFIED_CONSUMER_COMPILER_ARGS
                "-DUCN_TEST_RC_COMPILER=${CMAKE_RC_COMPILER}")
        endif()
    endif()

    add_test(NAME v6_install_consumer_gate
        COMMAND ${CMAKE_COMMAND}
            -DUCN_SOURCE_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DUCN_CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/v6/install_consumer
            -DUCN_CONSUMER_WORK_DIR=${UCN_CONSUMER_WORK_DIR}
            -DUCN_TEST_GENERATOR=${CMAKE_GENERATOR}
            -DUCN_TEST_CONFIGURATION=$<CONFIG>
            -DUCN_EXPECT_REALTIME=${UCN_FEATURE_REALTIME}
            -DUCN_EXPECT_CLUSTER=${UCN_FEATURE_CLUSTER}
            -DUCN_EXPECT_ADAPTER=${UCN_FEATURE_ADAPTER}
            "-DUCN_TEST_C_FLAGS=${CMAKE_C_FLAGS}"
            "-DUCN_TEST_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/test_v6_install_consumer.cmake)
    add_test(NAME v6s_install_consumer_gate
        COMMAND ${CMAKE_COMMAND}
            -DUCN_SOURCE_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DUCN_CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/simplified/install_consumer
            -DUCN_CONSUMER_WORK_DIR=${UCN_SIMPLIFIED_CONSUMER_WORK_DIR}
            -DUCN_TEST_GENERATOR=${CMAKE_GENERATOR}
            -DUCN_TEST_CONFIGURATION=$<CONFIG>
            ${UCN_SIMPLIFIED_CONSUMER_COMPILER_ARGS}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/test_v6s_install_consumer.cmake)
    if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
        add_test(NAME v6s_ascii_staging_contract_gate
            COMMAND ${CMAKE_COMMAND}
                -DUCN_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -DUCN_TEST_WORK_ROOT=${UCN_VALIDATED_ASCII_STAGING_ROOT}/ucn-v6s-ascii-contract-${UCN_CONSUMER_WORK_HASH}
                -DUCN_TEST_GENERATOR=${CMAKE_GENERATOR}
                -DUCN_TEST_C_COMPILER=${CMAKE_C_COMPILER}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/test_v6s_ascii_staging.cmake)
    endif()
endif()
