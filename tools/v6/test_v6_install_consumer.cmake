if(NOT DEFINED UCN_SOURCE_BUILD_DIR OR
   NOT DEFINED UCN_CONSUMER_SOURCE_DIR OR
   NOT DEFINED UCN_CONSUMER_WORK_DIR OR
   NOT DEFINED UCN_TEST_GENERATOR OR
   NOT DEFINED UCN_TEST_CONFIGURATION OR
   NOT DEFINED UCN_EXPECT_REALTIME OR
   NOT DEFINED UCN_EXPECT_CLUSTER OR
   NOT DEFINED UCN_EXPECT_ADAPTER)
    message(FATAL_ERROR "v6 install-consumer gate arguments are incomplete")
endif()

file(REMOVE_RECURSE "${UCN_CONSUMER_WORK_DIR}")
set(install_dir "${UCN_CONSUMER_WORK_DIR}/install")
set(build_dir "${UCN_CONSUMER_WORK_DIR}/build")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${UCN_SOURCE_BUILD_DIR}"
            --config "${UCN_TEST_CONFIGURATION}"
            --prefix "${install_dir}"
    RESULT_VARIABLE install_result)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "UCN v6 install step failed: ${install_result}")
endif()

function(ucn_check_optional_header header_name expected)
    set(header_path "${install_dir}/include/ucn/v6/${header_name}")
    if(expected AND NOT EXISTS "${header_path}")
        message(FATAL_ERROR "enabled v6 feature header was not installed: ${header_name}")
    elseif(NOT expected AND EXISTS "${header_path}")
        message(FATAL_ERROR "disabled v6 feature header leaked into install: ${header_name}")
    endif()
endfunction()

ucn_check_optional_header("ucn_v6_realtime.h" "${UCN_EXPECT_REALTIME}")
ucn_check_optional_header("ucn_v6_cluster.h" "${UCN_EXPECT_CLUSTER}")
ucn_check_optional_header("ucn_v6_adapter.h" "${UCN_EXPECT_ADAPTER}")

foreach(adapter_directory adapters ports reference)
    set(adapter_path "${install_dir}/include/ucn/v6/${adapter_directory}")
    if(UCN_EXPECT_ADAPTER AND NOT IS_DIRECTORY "${adapter_path}")
        message(FATAL_ERROR "enabled v6 Adapter directory was not installed: ${adapter_directory}")
    elseif(NOT UCN_EXPECT_ADAPTER AND EXISTS "${adapter_path}")
        message(FATAL_ERROR "disabled v6 Adapter directory leaked into install: ${adapter_directory}")
    endif()
endforeach()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -S "${UCN_CONSUMER_SOURCE_DIR}"
            -B "${build_dir}"
            -G "${UCN_TEST_GENERATOR}"
            "-DCMAKE_PREFIX_PATH=${install_dir}"
            "-DCMAKE_BUILD_TYPE=${UCN_TEST_CONFIGURATION}"
            "-DCMAKE_C_FLAGS=${UCN_TEST_C_FLAGS}"
            "-DCMAKE_EXE_LINKER_FLAGS=${UCN_TEST_EXE_LINKER_FLAGS}"
    RESULT_VARIABLE configure_result)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "UCN v6 package consumer configure failed: ${configure_result}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${build_dir}"
            --config "${UCN_TEST_CONFIGURATION}"
    RESULT_VARIABLE build_result)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "UCN v6 package consumer build failed: ${build_result}")
endif()

execute_process(
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${build_dir}"
            --build-config "${UCN_TEST_CONFIGURATION}"
            --output-on-failure
    RESULT_VARIABLE test_result)
if(NOT test_result EQUAL 0)
    message(FATAL_ERROR "UCN v6 package consumer execution failed: ${test_result}")
endif()

message(STATUS "UCN v6 install consumer linked and ran through UCN::ucn")
