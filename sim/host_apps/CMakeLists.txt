# Copyright 2026 siraisisatoru
#
# SPDX-License-Identifier: Apache-2.0
#
# Host builds of the three lab nodes, linking common/ with its POSIX port:
#   cmake -S sim/host_apps -B /tmp/simbuild && cmake --build /tmp/simbuild -j
cmake_minimum_required(VERSION 3.16)
project(halow_lab_sim_apps C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(LAB_SANITIZE "Build with -fsanitize=address,undefined" OFF)
if(LAB_SANITIZE)
    # common/ adds the same flags in its own scope; the apps must link the runtimes too.
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

set(LAB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../common ${CMAKE_CURRENT_BINARY_DIR}/common)

set(SIM_WARN_FLAGS -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2
                   -Werror)

add_library(host_app_util STATIC host_app_util.c)
target_compile_options(host_app_util PRIVATE ${SIM_WARN_FLAGS})
target_compile_definitions(host_app_util PUBLIC _POSIX_C_SOURCE=200809L)
target_link_libraries(host_app_util PUBLIC halow_lab_common)

foreach(app node_a_host node_b_host relay_host)
    add_executable(${app} ${app}.c)
    target_compile_options(${app} PRIVATE ${SIM_WARN_FLAGS})
    target_link_libraries(${app} PRIVATE host_app_util)
endforeach()
