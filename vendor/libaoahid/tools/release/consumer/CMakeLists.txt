# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
cmake_minimum_required(VERSION 3.20)
project(aoahid_consumer_smoke LANGUAGES C)

set(AOAHID_FIND_COMPONENTS "" CACHE STRING
    "Semicolon-separated installed aoahid components requested by the smoke consumer.")
if(AOAHID_FIND_COMPONENTS)
  find_package(aoahid CONFIG REQUIRED COMPONENTS ${AOAHID_FIND_COMPONENTS})
else()
  find_package(aoahid CONFIG REQUIRED)
endif()

set(AOAHID_CONSUMER_TARGET "aoahid::aoahid" CACHE STRING
    "Installed libaoahid target exercised by this smoke consumer.")
if(NOT TARGET "${AOAHID_CONSUMER_TARGET}")
  message(FATAL_ERROR
      "Requested installed target does not exist: ${AOAHID_CONSUMER_TARGET}")
endif()

add_executable(aoahid_consumer main.c)
target_link_libraries(aoahid_consumer PRIVATE "${AOAHID_CONSUMER_TARGET}")
set_target_properties(aoahid_consumer PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES)
