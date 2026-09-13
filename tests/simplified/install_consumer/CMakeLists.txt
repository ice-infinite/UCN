cmake_minimum_required(VERSION 3.20)
project(UCNSimplifiedInstallConsumer LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(UCN 6 CONFIG REQUIRED)
add_executable(ucn_simplified_install_consumer main.c)
target_link_libraries(ucn_simplified_install_consumer PRIVATE UCN::simplified)
add_executable(ucn_simplified_install_consumer_cpp main.cpp)
target_link_libraries(ucn_simplified_install_consumer_cpp PRIVATE UCN::simplified)
if(MSVC)
    target_compile_options(ucn_simplified_install_consumer PRIVATE
        /W4 /WX /utf-8)
    target_compile_options(ucn_simplified_install_consumer_cpp PRIVATE
        /W4 /WX /utf-8)
else()
    target_compile_options(ucn_simplified_install_consumer PRIVATE
        -Wall -Wextra -Werror -pedantic -fshort-enums)
    target_compile_options(ucn_simplified_install_consumer_cpp PRIVATE
        -Wall -Wextra -Werror -pedantic -fshort-enums)
endif()
enable_testing()
add_test(NAME ucn_simplified_install_consumer_runs
         COMMAND ucn_simplified_install_consumer)
add_test(NAME ucn_simplified_install_consumer_cpp_runs
         COMMAND ucn_simplified_install_consumer_cpp)
