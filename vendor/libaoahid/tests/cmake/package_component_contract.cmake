# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
if(NOT DEFINED AOAHID_SOURCE_DIR OR NOT DEFINED AOAHID_TEST_BINARY_DIR OR
   NOT DEFINED AOAHID_GENERATOR OR NOT DEFINED AOAHID_PROJECT_VERSION OR
   NOT DEFINED AOAHID_PROJECT_HOMEPAGE_URL)
  message(FATAL_ERROR "package component contract test inputs are incomplete")
endif()

# This is a configure-package contract test. It deliberately does not start a
# nested C/C++ toolchain: doing so made the test depend on the host/target
# pairing of the Visual Studio runner even though no object file is required to
# validate CMake component selection.
include(CMakePackageConfigHelpers)

set(_AOAHID_STAGE "${AOAHID_TEST_BINARY_DIR}/stage")
set(_AOAHID_PACKAGE_DIR "${_AOAHID_STAGE}/lib/cmake/aoahid")
set(_AOAHID_CONSUMER_SOURCE "${AOAHID_TEST_BINARY_DIR}/consumer")
file(REMOVE_RECURSE "${AOAHID_TEST_BINARY_DIR}")
file(MAKE_DIRECTORY "${_AOAHID_PACKAGE_DIR}" "${_AOAHID_CONSUMER_SOURCE}")

# Generate the exact static-only installed configuration directly from the
# production template. These are the same substitution values used by a
# fake-libusb static-only build; no compiler discovery is needed.
set(PROJECT_VERSION "${AOAHID_PROJECT_VERSION}")
set(PROJECT_HOMEPAGE_URL "${AOAHID_PROJECT_HOMEPAGE_URL}")
set(AOAHID_BUILD_SHARED OFF)
set(AOAHID_BUILD_STATIC ON)
set(AOAHID_USB_LINK_TARGET "")
set(CMAKE_INSTALL_INCLUDEDIR "include")
set(CMAKE_INSTALL_LIBDIR "lib")
configure_package_config_file(
    "${AOAHID_SOURCE_DIR}/cmake/aoahid-config.cmake.in"
    "${_AOAHID_PACKAGE_DIR}/aoahid-config.cmake"
    INSTALL_DESTINATION "lib/cmake/aoahid"
    PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR)
write_basic_package_version_file(
    "${_AOAHID_PACKAGE_DIR}/aoahid-config-version.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)
file(COPY "${AOAHID_SOURCE_DIR}/cmake/aoahid-check-libusb.cmake"
    DESTINATION "${_AOAHID_PACKAGE_DIR}")

# Deliberately place both exports in the stage. The embedded OFF/ON build
# contract must still reject shared and accept static.
file(WRITE "${AOAHID_TEST_BINARY_DIR}/dummy-static.lib" "")
file(WRITE "${_AOAHID_PACKAGE_DIR}/aoahid-shared-targets.cmake"
    "add_library(aoahid::aoahid SHARED IMPORTED)\n")
file(WRITE "${_AOAHID_PACKAGE_DIR}/aoahid-static-targets.cmake"
    "add_library(aoahid::aoahid_static STATIC IMPORTED)\n"
    "set_target_properties(aoahid::aoahid_static PROPERTIES "
    "IMPORTED_LOCATION \"${AOAHID_TEST_BINARY_DIR}/dummy-static.lib\")\n")

# LANGUAGES NONE is intentional. Component selection is CMake package metadata
# behavior, so the test must behave identically on x64 and ARM64.
file(WRITE "${_AOAHID_CONSUMER_SOURCE}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.21)
project(aoahid_component_contract LANGUAGES NONE)
if(NOT DEFINED AOAHID_FIND_COMPONENTS)
  message(FATAL_ERROR "AOAHID_FIND_COMPONENTS is required")
endif()
find_package(aoahid CONFIG REQUIRED COMPONENTS ${AOAHID_FIND_COMPONENTS})
if(DEFINED AOAHID_EXPECT_TARGET AND NOT TARGET "${AOAHID_EXPECT_TARGET}")
  message(FATAL_ERROR "Expected installed target does not exist: ${AOAHID_EXPECT_TARGET}")
endif()
]=])

set(_AOAHID_CONSUMER_BASE
    "${CMAKE_COMMAND}"
    -S "${_AOAHID_CONSUMER_SOURCE}"
    -G "${AOAHID_GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${_AOAHID_STAGE}")

execute_process(
    COMMAND ${_AOAHID_CONSUMER_BASE}
        -B "${AOAHID_TEST_BINARY_DIR}/wrong-shared"
        -DAOAHID_FIND_COMPONENTS=shared
    RESULT_VARIABLE _AOAHID_WRONG_SHARED_RESULT
    OUTPUT_VARIABLE _AOAHID_WRONG_SHARED_STDOUT
    ERROR_VARIABLE _AOAHID_WRONG_SHARED_STDERR)
if(_AOAHID_WRONG_SHARED_RESULT EQUAL 0)
  message(FATAL_ERROR
      "static-only package accepted shared despite its embedded build contract\n"
      "${_AOAHID_WRONG_SHARED_STDOUT}\n${_AOAHID_WRONG_SHARED_STDERR}")
endif()

execute_process(
    COMMAND ${_AOAHID_CONSUMER_BASE}
        -B "${AOAHID_TEST_BINARY_DIR}/static"
        -DAOAHID_FIND_COMPONENTS=static
        -DAOAHID_EXPECT_TARGET=aoahid::aoahid_static
    RESULT_VARIABLE _AOAHID_STATIC_RESULT
    OUTPUT_VARIABLE _AOAHID_STATIC_STDOUT
    ERROR_VARIABLE _AOAHID_STATIC_STDERR)
if(NOT _AOAHID_STATIC_RESULT EQUAL 0)
  message(FATAL_ERROR
      "static-only package rejected its static component (${_AOAHID_STATIC_RESULT})\n"
      "${_AOAHID_STATIC_STDOUT}\n${_AOAHID_STATIC_STDERR}")
endif()
