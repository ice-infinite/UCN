if(NOT DEFINED UCN_SOURCE_BUILD_DIR OR
   NOT DEFINED UCN_CONSUMER_SOURCE_DIR OR
   NOT DEFINED UCN_CONSUMER_WORK_DIR OR
   NOT DEFINED UCN_TEST_GENERATOR OR
   NOT DEFINED UCN_TEST_CONFIGURATION)
    message(FATAL_ERROR "simplified install-consumer arguments are incomplete")
endif()

file(REMOVE_RECURSE "${UCN_CONSUMER_WORK_DIR}")
set(install_dir "${UCN_CONSUMER_WORK_DIR}/install")
set(build_dir "${UCN_CONSUMER_WORK_DIR}/build")
set(compiler_args)
if(DEFINED UCN_TEST_C_COMPILER AND NOT UCN_TEST_C_COMPILER STREQUAL "")
    list(APPEND compiler_args
        "-DCMAKE_C_COMPILER=${UCN_TEST_C_COMPILER}")
endif()
if(DEFINED UCN_TEST_CXX_COMPILER AND NOT UCN_TEST_CXX_COMPILER STREQUAL "")
    list(APPEND compiler_args
        "-DCMAKE_CXX_COMPILER=${UCN_TEST_CXX_COMPILER}")
endif()
if(DEFINED UCN_TEST_RC_COMPILER AND NOT UCN_TEST_RC_COMPILER STREQUAL "")
    list(APPEND compiler_args
        "-DCMAKE_RC_COMPILER=${UCN_TEST_RC_COMPILER}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${UCN_SOURCE_BUILD_DIR}"
            --config "${UCN_TEST_CONFIGURATION}" --prefix "${install_dir}"
    RESULT_VARIABLE install_result)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "simplified install step failed: ${install_result}")
endif()

foreach(header ucn_simplified.h ucn_types.h ucn_config.h ucn_driver.h ucn_product.h ucn_core.h)
    if(NOT EXISTS "${install_dir}/include/ucn/${header}")
        message(FATAL_ERROR "missing simplified public header: ${header}")
    endif()
endforeach()
file(GLOB_RECURSE forbidden
    "${install_dir}/*ucn_common*" "${install_dir}/*ucn_coordinator*"
    "${install_dir}/*ucn_wire.*" "${install_dir}/*ucn_adapter.*"
    "${install_dir}/*ucn_kernel*")
if(forbidden)
    message(FATAL_ERROR "simplified internal archive leaked: ${forbidden}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${UCN_CONSUMER_SOURCE_DIR}"
            -B "${build_dir}" -G "${UCN_TEST_GENERATOR}"
            "-DCMAKE_PREFIX_PATH=${install_dir}"
            "-DCMAKE_BUILD_TYPE=${UCN_TEST_CONFIGURATION}"
            ${compiler_args}
    RESULT_VARIABLE configure_result)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "simplified consumer configure failed: ${configure_result}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${build_dir}"
            --config "${UCN_TEST_CONFIGURATION}"
    RESULT_VARIABLE build_result)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "simplified consumer build failed: ${build_result}")
endif()
execute_process(
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${build_dir}"
            --build-config "${UCN_TEST_CONFIGURATION}" --output-on-failure
    RESULT_VARIABLE test_result)
if(NOT test_result EQUAL 0)
    message(FATAL_ERROR "simplified consumer run failed: ${test_result}")
endif()

message(STATUS "simplified install consumer linked and ran through UCN::simplified")
