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
set(UCN_PROFILE "FULL" CACHE STRING "UCN v6 resource profile: NANO, LITE, or FULL")
set_property(CACHE UCN_PROFILE PROPERTY STRINGS NANO LITE FULL)
set(UCN_USER_CONFIG_HEADER "" CACHE STRING
    "Optional product configuration header included before v6 defaults")
set(UCN_USER_CONFIG_INCLUDE_DIR "" CACHE PATH
    "Include directory containing UCN_USER_CONFIG_HEADER")

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
    "cluster=${UCN_FEATURE_CLUSTER} adapter=${UCN_FEATURE_ADAPTER}")

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
    ucn_v6_config ucn_v6_identity ucn_v6_wire ucn_v6_message ucn_v6_owner
    ucn_v6_security ucn_v6_capability ucn_v6_route ucn_v6_qos
    ucn_v6_transfer)

if(UCN_FEATURE_REALTIME)
    add_library(ucn_v6_realtime STATIC src/v6/realtime/ucn_v6_realtime.c)
    ucn_target_defaults(ucn_v6_realtime)
    target_link_libraries(ucn_v6_realtime PUBLIC
        ucn_v6_config ucn_v6_wire ucn_v6_security ucn_v6_capability)
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
    target_link_libraries(ucn INTERFACE ucn_v6_adapter)
endif()
list(APPEND UCN_EXPORT_TARGETS ucn)

install(TARGETS ${UCN_EXPORT_TARGETS}
    EXPORT UCNTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/ucn/ucn.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ucn)
install(DIRECTORY include/ucn/v6 DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ucn)
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
    add_test(NAME v6_archive_symbol_gate
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_archives.py
            --build-dir ${CMAKE_CURRENT_BINARY_DIR}
            --symbol-tool ${UCN_SYMBOL_TOOL})
    add_test(NAME v6_current_docs_gate
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_current_docs.py
            --root ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME v6_api_index_gate
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/generate_v6_api_index.py
            --root ${CMAKE_CURRENT_SOURCE_DIR}
            --output ${CMAKE_CURRENT_SOURCE_DIR}/docs/源码阅读指南/06-公共函数签名索引.md
            --check)
    add_test(NAME v6_removed_cmake_surface_gate
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/check_v6_removed_cmake_surface.py
            --cmake ${CMAKE_COMMAND}
            --source ${CMAKE_CURRENT_SOURCE_DIR}
            --work ${CMAKE_CURRENT_BINARY_DIR}/v6-removed-cmake-gate
            --generator ${CMAKE_GENERATOR})

    ucn_add_test(ucn_v6_config_contract_tests
        tests/v6/test_v6_config_contract.c ucn_v6_config)
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
    # characters even though CMake/Ninja can build and install them.  Stage the
    # package-consumer link in a deterministic ASCII temporary directory on
    # Windows; the source build itself may still live in a Unicode path.
    if(WIN32 AND DEFINED ENV{TEMP})
        string(SHA256 UCN_CONSUMER_WORK_HASH "${CMAKE_CURRENT_BINARY_DIR}")
        string(SUBSTRING "${UCN_CONSUMER_WORK_HASH}" 0 16 UCN_CONSUMER_WORK_HASH)
        file(TO_CMAKE_PATH "$ENV{TEMP}" UCN_HOST_TEMP_DIR)
        set(UCN_CONSUMER_WORK_DIR
            "${UCN_HOST_TEMP_DIR}/ucn-v6-consumer-${UCN_CONSUMER_WORK_HASH}")
    else()
        set(UCN_CONSUMER_WORK_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/v6-install-consumer")
    endif()

    add_test(NAME v6_install_consumer_gate
        COMMAND ${CMAKE_COMMAND}
            -DUCN_SOURCE_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DUCN_CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/v6/install_consumer
            -DUCN_CONSUMER_WORK_DIR=${UCN_CONSUMER_WORK_DIR}
            -DUCN_TEST_GENERATOR=${CMAKE_GENERATOR}
            "-DUCN_TEST_C_FLAGS=${CMAKE_C_FLAGS}"
            "-DUCN_TEST_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/v6/test_v6_install_consumer.cmake)
endif()
