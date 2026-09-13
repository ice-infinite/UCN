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
option(UCN_FEATURE_CLUSTER "Build the optional v6 Cluster module" ON)
option(UCN_FEATURE_ADAPTER "Build v6 Adapter, bearer, RTOS, and reference ports" ON)
option(UCN_FEATURE_PERSISTENCE
    "Build the optional simplified Persistence Foundation" OFF)
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

foreach(feature REALTIME CLUSTER ADAPTER)
    if(UCN_FEATURE_${feature})
        set(UCN_FEATURE_${feature}_VALUE 1)
    else()
        set(UCN_FEATURE_${feature}_VALUE 0)
    endif()
endforeach()

message(STATUS
    "UCN v6 profile=${UCN_PROFILE_UPPER} realtime=${UCN_FEATURE_REALTIME} "
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
target_compile_definitions(ucn_kernel PUBLIC UCN_PROFILE=${UCN_PROFILE_VALUE})
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
    if(UCN_FEATURE_PERSISTENCE)
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
