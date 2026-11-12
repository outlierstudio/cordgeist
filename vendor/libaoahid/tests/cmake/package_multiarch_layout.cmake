# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
if(NOT DEFINED AOAHID_SOURCE_DIR OR NOT DEFINED AOAHID_TEST_BINARY_DIR OR
   NOT DEFINED AOAHID_GENERATOR)
  message(FATAL_ERROR "package layout test inputs are incomplete")
endif()

set(_AOAHID_CONFIGURE_COMMAND
    "${CMAKE_COMMAND}"
    -S "${AOAHID_SOURCE_DIR}"
    -B "${AOAHID_TEST_BINARY_DIR}"
    -G "${AOAHID_GENERATOR}"
    -DAOAHID_USE_FAKE_LIBUSB=ON
    -DAOAHID_BUILD_SHARED=ON
    -DAOAHID_BUILD_STATIC=ON
    -DAOAHID_BUILD_TESTS=OFF
    -DAOAHID_BUILD_EXAMPLES=OFF
    -DCMAKE_INSTALL_LIBDIR=lib/x86_64-linux-gnu)
if(DEFINED AOAHID_GENERATOR_PLATFORM AND
   NOT AOAHID_GENERATOR_PLATFORM STREQUAL "")
  list(APPEND _AOAHID_CONFIGURE_COMMAND -A "${AOAHID_GENERATOR_PLATFORM}")
endif()
if(DEFINED AOAHID_GENERATOR_TOOLSET AND
   NOT AOAHID_GENERATOR_TOOLSET STREQUAL "")
  list(APPEND _AOAHID_CONFIGURE_COMMAND -T "${AOAHID_GENERATOR_TOOLSET}")
endif()

execute_process(
    COMMAND ${_AOAHID_CONFIGURE_COMMAND}
    RESULT_VARIABLE _AOAHID_CONFIGURE_RESULT
    OUTPUT_VARIABLE _AOAHID_CONFIGURE_STDOUT
    ERROR_VARIABLE _AOAHID_CONFIGURE_STDERR)
if(NOT _AOAHID_CONFIGURE_RESULT EQUAL 0)
  message(FATAL_ERROR
      "nested multiarch configure failed (${_AOAHID_CONFIGURE_RESULT})\n"
      "${_AOAHID_CONFIGURE_STDOUT}\n${_AOAHID_CONFIGURE_STDERR}")
endif()

function(aoahid_require_contains contents expected label)
  string(FIND "${contents}" "${expected}" _AOAHID_MATCH_INDEX)
  if(_AOAHID_MATCH_INDEX EQUAL -1)
    message(FATAL_ERROR "${label} does not contain expected text: ${expected}")
  endif()
endfunction()

file(READ "${AOAHID_TEST_BINARY_DIR}/aoahid.pc" _AOAHID_PC)
aoahid_require_contains(
    "${_AOAHID_PC}" "prefix=\${pcfiledir}/../../.." "multiarch aoahid.pc")
aoahid_require_contains(
    "${_AOAHID_PC}"
    "libdir=\${exec_prefix}/lib/x86_64-linux-gnu"
    "multiarch aoahid.pc")
aoahid_require_contains(
    "${_AOAHID_PC}" "includedir=\${prefix}/include" "multiarch aoahid.pc")

file(READ "${AOAHID_TEST_BINARY_DIR}/aoahid-config.cmake" _AOAHID_CONFIG)
aoahid_require_contains(
    "${_AOAHID_CONFIG}"
    [=[get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../" ABSOLUTE)]=]
    "multiarch CMake package config")
aoahid_require_contains(
    "${_AOAHID_CONFIG}"
    [=[HINTS "${PACKAGE_PREFIX_DIR}/lib/x86_64-linux-gnu"]=]
    "multiarch CMake package dependency hint")
