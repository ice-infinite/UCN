cmake_minimum_required(VERSION 3.20)
project(UCNV6InstallConsumer LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(UCN 6 CONFIG REQUIRED)

add_executable(ucn_v6_install_consumer main.c)
target_link_libraries(ucn_v6_install_consumer PRIVATE UCN::ucn)

enable_testing()
add_test(NAME ucn_v6_install_consumer_runs COMMAND ucn_v6_install_consumer)
