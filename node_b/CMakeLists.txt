# Copyright 2026 siraisisatoru
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.16)

# Shared lab protocol and traffic services (component "common").
set(EXTRA_COMPONENT_DIRS ../common)

# Local, git-ignored lab settings (SSID, passphrase, board BSSIDs): ../lab_local.conf, see
# ../lab_local.conf.example. Added after any other defaults, so it wins. Defaults only fill a new
# sdkconfig, so warn when the file is newer than the existing one.
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
endif()
set(LAB_LOCAL_CONF "${CMAKE_CURRENT_LIST_DIR}/../lab_local.conf")
if(EXISTS "${LAB_LOCAL_CONF}")
    list(APPEND SDKCONFIG_DEFAULTS "${LAB_LOCAL_CONF}")
    if(DEFINED SDKCONFIG)
        set(_lab_sdkconfig "${SDKCONFIG}")
    else()
        set(_lab_sdkconfig "${CMAKE_CURRENT_LIST_DIR}/sdkconfig")
    endif()
    if(EXISTS "${_lab_sdkconfig}" AND "${LAB_LOCAL_CONF}" IS_NEWER_THAN "${_lab_sdkconfig}")
        message(WARNING "lab_local.conf is newer than ${_lab_sdkconfig}: values already in "
                        "sdkconfig are kept. Delete sdkconfig (or idf.py fullclean) to apply it.")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(node_b)
