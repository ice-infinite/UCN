if(NOT DEFINED UCN_SOURCE_BUILD_DIR OR
   NOT DEFINED UCN_CONSUMER_SOURCE_DIR OR
   NOT DEFINED UCN_CONSUMER_WORK_DIR OR
   NOT DEFINED UCN_TEST_GENERATOR)
    message(FATAL_ERROR "v6 install-consumer gate arguments are incomplete")
endif()

file(REMOVE_RECURSE "${UCN_CONSUMER_WORK_DIR}")
set(install_dir "${UCN_CONSUMER_WORK_DIR}/install")
set(build_dir "${UCN_CONSUMER_WORK_DIR}/build")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${UCN_SOURCE_BUILD_DIR}"
            --prefix "${install_dir}"
    RESULT_VARIABLE install_result)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "UCN v6 install step failed: ${install_result}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -S "${UCN_CONSUMER_SOURCE_DIR}"
            -B "${build_dir}"
            -G "${UCN_TEST_GENERATOR}"
            "-DCMAKE_PREFIX_PATH=${install_dir}"
            "-DCMAKE_C_FLAGS=${UCN_TEST_C_FLAGS}"
            "-DCMAKE_EXE_LINKER_FLAGS=${UCN_TEST_EXE_LINKER_FLAGS}"
    RESULT_VARIABLE configure_result)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "UCN v6 package consumer configure failed: ${configure_result}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${build_dir}"
    RESULT_VARIABLE build_result)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "UCN v6 package consumer build failed: ${build_result}")
endif()

execute_process(
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${build_dir}"
    RESULT_VARIABLE ignored_test_result
    OUTPUT_QUIET
    ERROR_QUIET)

message(STATUS "UCN v6 install consumer linked through UCN::ucn")
