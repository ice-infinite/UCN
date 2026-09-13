if(NOT DEFINED UCN_SOURCE_DIR OR NOT DEFINED UCN_TEST_WORK_ROOT OR
   NOT DEFINED UCN_TEST_GENERATOR OR NOT DEFINED UCN_TEST_C_COMPILER)
    message(FATAL_ERROR "v6s ASCII staging self-test is missing an argument")
endif()

file(REMOVE_RECURSE "${UCN_TEST_WORK_ROOT}")
file(MAKE_DIRECTORY "${UCN_TEST_WORK_ROOT}")
set(UCN_TEST_UNICODE_TEMP "${UCN_TEST_WORK_ROOT}/中文-temp")
set(UCN_TEST_ASCII_ROOT "${UCN_TEST_WORK_ROOT}/accepted-stage")
file(MAKE_DIRECTORY "${UCN_TEST_UNICODE_TEMP}" "${UCN_TEST_ASCII_ROOT}")

set(UCN_SAVED_TEMP "$ENV{TEMP}")
set(UCN_SAVED_TMP "$ENV{TMP}")
set(ENV{TEMP} "${UCN_TEST_UNICODE_TEMP}")
set(ENV{TMP} "${UCN_TEST_UNICODE_TEMP}")

set(UCN_COMMON_CONFIG_ARGS
    -G "${UCN_TEST_GENERATOR}"
    -DCMAKE_C_COMPILER=${UCN_TEST_C_COMPILER}
    -DUCN_BUILD_TESTS=ON
    -DUCN_PROFILE=NANO
    -DUCN_FEATURE_REALTIME=OFF
    -DUCN_FEATURE_CLUSTER=OFF
    -DUCN_FEATURE_ADAPTER=OFF)

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${UCN_SOURCE_DIR}"
        -B "${UCN_TEST_WORK_ROOT}/reject" ${UCN_COMMON_CONFIG_ARGS}
    RESULT_VARIABLE UCN_REJECT_RESULT
    OUTPUT_VARIABLE UCN_REJECT_OUTPUT
    ERROR_VARIABLE UCN_REJECT_ERROR)
if(UCN_REJECT_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Unicode TEMP unexpectedly passed without UCN_ASCII_STAGING_ROOT")
endif()
string(CONCAT UCN_REJECT_LOG "${UCN_REJECT_OUTPUT}" "${UCN_REJECT_ERROR}")
if(NOT UCN_REJECT_LOG MATCHES "UCN_ASCII_STAGING_ROOT")
    message(FATAL_ERROR
        "Unicode TEMP rejection did not provide staging-root guidance")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${UCN_SOURCE_DIR}"
        -B "${UCN_TEST_WORK_ROOT}/accept" ${UCN_COMMON_CONFIG_ARGS}
        -DUCN_ASCII_STAGING_ROOT=${UCN_TEST_ASCII_ROOT}
    RESULT_VARIABLE UCN_ACCEPT_RESULT
    OUTPUT_VARIABLE UCN_ACCEPT_OUTPUT
    ERROR_VARIABLE UCN_ACCEPT_ERROR)

if(NOT UCN_ACCEPT_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Explicit ASCII staging root was rejected:\n"
        "${UCN_ACCEPT_OUTPUT}\n${UCN_ACCEPT_ERROR}")
endif()

# Prove the override through the real installed-package C and C++ consumers,
# not merely through CMake configuration.  Only the simplified archive is
# needed; the consumer gate performs install, include, link, and execution.
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${UCN_TEST_WORK_ROOT}/accept"
    RESULT_VARIABLE UCN_ACCEPT_BUILD_RESULT
    OUTPUT_VARIABLE UCN_ACCEPT_BUILD_OUTPUT
    ERROR_VARIABLE UCN_ACCEPT_BUILD_ERROR)
if(NOT UCN_ACCEPT_BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Explicit ASCII staging consumer prerequisite failed:\n"
        "${UCN_ACCEPT_BUILD_OUTPUT}\n${UCN_ACCEPT_BUILD_ERROR}")
endif()

execute_process(
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir
        "${UCN_TEST_WORK_ROOT}/accept" --output-on-failure
        -R "^v6s_install_consumer_gate$"
    RESULT_VARIABLE UCN_ACCEPT_CONSUMER_RESULT
    OUTPUT_VARIABLE UCN_ACCEPT_CONSUMER_OUTPUT
    ERROR_VARIABLE UCN_ACCEPT_CONSUMER_ERROR)

set(ENV{TEMP} "${UCN_SAVED_TEMP}")
set(ENV{TMP} "${UCN_SAVED_TMP}")

if(NOT UCN_ACCEPT_CONSUMER_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Explicit ASCII staging C/C++ consumer failed:\n"
        "${UCN_ACCEPT_CONSUMER_OUTPUT}\n${UCN_ACCEPT_CONSUMER_ERROR}")
endif()

message(STATUS
    "V6S_ASCII_STAGING_OK: unicode_default_rejected=1 "
    "explicit_ascii_consumer_passed=1")
