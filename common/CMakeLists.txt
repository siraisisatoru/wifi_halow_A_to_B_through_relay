# Copyright 2026 siraisisatoru
#
# SPDX-License-Identifier: Apache-2.0
#
# Two personalities:
#  - ESP-IDF component (ESP_PLATFORM set by idf.py): add this directory to EXTRA_COMPONENT_DIRS
#    and REQUIRES common.
#  - Plain CMake (host): static library halow_lab_common + lab_host tool + unit tests.
#      cmake -S common -B build -DLAB_SANITIZE=ON && cmake --build build && ctest --test-dir build

set(LAB_COMMON_SRCS
    src/lab_platform_common.c
    src/lab_sock.c
    src/lab_util.c
    src/linkproto.c
    src/linkstats.c
    src/msgstream.c
    src/svc_tcp_stream.c
    src/svc_udp_echo.c
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${LAB_COMMON_SRCS} port/esp_idf/lab_platform_esp.c port/esp_idf/ping_probe.c
                     port/esp_idf/tput.c
        INCLUDE_DIRS include port/esp_idf/include
        PRIV_INCLUDE_DIRS src
        REQUIRES lwip esp_timer log freertos
        PRIV_REQUIRES esp_hw_support
    )
    return()
endif()

cmake_minimum_required(VERSION 3.16)
project(halow_lab_common C)

option(LAB_WERROR "Treat warnings as errors" ON)
option(LAB_SANITIZE "Build with -fsanitize=address,undefined" OFF)
option(LAB_BUILD_TESTS "Build unit tests and the loopback integration test" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(LAB_WARN_FLAGS -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2)
if(LAB_WERROR)
    list(APPEND LAB_WARN_FLAGS -Werror)
endif()
if(LAB_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

add_library(halow_lab_common STATIC ${LAB_COMMON_SRCS} port/posix/lab_platform_posix.c)
target_include_directories(halow_lab_common PUBLIC include PRIVATE src)
target_compile_options(halow_lab_common PRIVATE ${LAB_WARN_FLAGS})
# POSIX socket/time APIs under -std=c11.
target_compile_definitions(halow_lab_common PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
target_link_libraries(halow_lab_common PUBLIC Threads::Threads)

add_executable(lab_host host/lab_host.c)
target_compile_options(lab_host PRIVATE ${LAB_WARN_FLAGS})
target_compile_definitions(lab_host PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(lab_host PRIVATE halow_lab_common)

if(LAB_BUILD_TESTS)
    enable_testing()
    add_executable(lab_unit_tests
        test/test_main.c
        test/test_linkproto.c
        test/test_linkstats.c
        test/test_msgstream.c
    )
    target_compile_options(lab_unit_tests PRIVATE ${LAB_WARN_FLAGS})
    target_link_libraries(lab_unit_tests PRIVATE halow_lab_common)
    add_test(NAME unit COMMAND lab_unit_tests)

    add_test(NAME loopback
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/test_loopback.sh $<TARGET_FILE:lab_host>)
    set_tests_properties(loopback PROPERTIES TIMEOUT 120)
endif()
