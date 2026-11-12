# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
cmake_minimum_required(VERSION 3.20)

project(libaoahid VERSION 2.0.3
    DESCRIPTION "Explicit host-side Android Open Accessory HID input library"
    HOMEPAGE_URL "https://github.com/nemarpuc/libaoahid"
    LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(cmake/aoahid-check-libusb.cmake)

option(AOAHID_WERROR "Treat compiler warnings as errors." ON)
option(AOAHID_BUILD_SHARED "Build the shared library." ON)
option(AOAHID_BUILD_STATIC "Build the static library." ON)
option(AOAHID_BUILD_TESTS "Build tests." OFF)
option(AOAHID_BUILD_EXAMPLES "Build examples." OFF)
option(AOAHID_BUILD_FUZZ "Build fuzz targets." OFF)
option(AOAHID_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer." OFF)
option(AOAHID_TSAN "Enable ThreadSanitizer for race detection." OFF)
option(AOAHID_CHECK_SERIALIZATION "Abort when two threads are inside calls on one Context (debugging aid)." OFF)
option(AOAHID_USE_FAKE_LIBUSB "Use the deterministic test-only libusb backend." OFF)

if(AOAHID_SANITIZE AND AOAHID_TSAN)
  message(FATAL_ERROR
      "AOAHID_SANITIZE and AOAHID_TSAN are mutually exclusive because "
      "ThreadSanitizer cannot be combined with AddressSanitizer.")
endif()
if(AOAHID_TSAN)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "AOAHID_TSAN is supported only for Linux host builds; "
        "CMAKE_SYSTEM_NAME is '${CMAKE_SYSTEM_NAME}'.")
  endif()
  if(NOT CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang)$" OR
     NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    message(FATAL_ERROR
        "AOAHID_TSAN requires GNU or Clang C and C++ compilers; found "
        "C='${CMAKE_C_COMPILER_ID}' and CXX='${CMAKE_CXX_COMPILER_ID}'.")
  endif()
  if((CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 12) OR
     (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12))
    message(FATAL_ERROR
        "AOAHID_TSAN requires GCC 12 or newer when using the GNU ThreadSanitizer runtime. "
        "GCC 11 has the unresolved condition-variable false positive tracked as GCC bug 101978.")
  endif()
  if(AOAHID_BUILD_FUZZ)
    message(FATAL_ERROR
        "AOAHID_TSAN cannot be combined with AOAHID_BUILD_FUZZ because the "
        "fuzz target uses the incompatible AddressSanitizer runtime.")
  endif()

  # TSan requires a usable compiler runtime and position-independent
  # executables.  Probe both languages because the project ships C consumers
  # as well as the C++ implementation, and fail during configuration instead
  # of leaving an obscure compiler or linker error for the build step.
  include(CheckPIESupported)
  check_pie_supported(
      LANGUAGES C CXX
      OUTPUT_VARIABLE AOAHID_TSAN_PIE_DIAGNOSTIC)
  if(NOT CMAKE_C_LINK_PIE_SUPPORTED OR NOT CMAKE_CXX_LINK_PIE_SUPPORTED)
    message(FATAL_ERROR
        "AOAHID_TSAN requires PIE support for C and C++ executables. "
        "CMake's PIE probe reported: ${AOAHID_TSAN_PIE_DIAGNOSTIC}")
  endif()
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)

  include(CheckCSourceCompiles)
  include(CheckCXXSourceCompiles)
  set(AOAHID_SAVED_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
  set(AOAHID_SAVED_REQUIRED_LINK_OPTIONS "${CMAKE_REQUIRED_LINK_OPTIONS}")
  string(APPEND CMAKE_REQUIRED_FLAGS " -fsanitize=thread")
  list(APPEND CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=thread)
  check_c_source_compiles(
      "int main(void) { return 0; }"
      AOAHID_C_TSAN_COMPILES_AND_LINKS)
  check_cxx_source_compiles(
      "int main() { return 0; }"
      AOAHID_CXX_TSAN_COMPILES_AND_LINKS)
  set(CMAKE_REQUIRED_FLAGS "${AOAHID_SAVED_REQUIRED_FLAGS}")
  set(CMAKE_REQUIRED_LINK_OPTIONS "${AOAHID_SAVED_REQUIRED_LINK_OPTIONS}")
  if(NOT AOAHID_C_TSAN_COMPILES_AND_LINKS OR
     NOT AOAHID_CXX_TSAN_COMPILES_AND_LINKS)
    message(FATAL_ERROR
        "AOAHID_TSAN was requested, but the selected C/C++ toolchain cannot "
        "compile and link against a usable ThreadSanitizer runtime.")
  endif()
endif()

set(AOAHID_INSTRUMENT_COMPILE_OPTIONS "")
set(AOAHID_INSTRUMENT_LINK_OPTIONS "")
if(AOAHID_SANITIZE AND NOT MSVC)
  set(AOAHID_INSTRUMENT_COMPILE_OPTIONS
      -fsanitize=address,undefined -fno-omit-frame-pointer)
  set(AOAHID_INSTRUMENT_LINK_OPTIONS -fsanitize=address,undefined)
elseif(AOAHID_TSAN)
  set(AOAHID_INSTRUMENT_COMPILE_OPTIONS
      -fsanitize=thread -fno-omit-frame-pointer)
  set(AOAHID_INSTRUMENT_LINK_OPTIONS -fsanitize=thread)
endif()

if(NOT AOAHID_BUILD_SHARED AND NOT AOAHID_BUILD_STATIC)
  message(FATAL_ERROR "At least one of AOAHID_BUILD_SHARED or AOAHID_BUILD_STATIC must be enabled.")
endif()
if(AOAHID_BUILD_TESTS AND NOT AOAHID_USE_FAKE_LIBUSB)
  message(FATAL_ERROR "AOAHID_BUILD_TESTS requires AOAHID_USE_FAKE_LIBUSB because the integration suite uses the deterministic fake backend.")
endif()

set(AOAHID_SOURCES
    src/api/c_api.cpp
    src/api/error_detail.cpp
    src/hid/item_writer.cpp
    src/hid/descriptor_builder.cpp
    src/hid/validator.cpp
    src/profiles/spec.cpp
    src/profiles/state.cpp
    src/transport/channel.cpp
    src/transport/discovery.cpp
    src/transport/port.cpp
    src/transport/transport.cpp)

if(AOAHID_USE_FAKE_LIBUSB)
  set(AOAHID_USB_LINK_TARGET "")
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(AOAHID_LIBUSB QUIET IMPORTED_TARGET "libusb-1.0>=1.0.30")
  endif()
  if(TARGET PkgConfig::AOAHID_LIBUSB)
    set(AOAHID_USB_LINK_TARGET PkgConfig::AOAHID_LIBUSB)
  else()
    find_path(LIBUSB_INCLUDE_DIR NAMES libusb.h PATH_SUFFIXES libusb-1.0)
    find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0)
    if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
      message(FATAL_ERROR "libusb 1.0.30 or newer headers and library were not found. Install matching libusb-1.0 development files or use AOAHID_USE_FAKE_LIBUSB for tests only.")
    endif()
    aoahid_check_libusb_api_version(
        "${LIBUSB_INCLUDE_DIR}/libusb.h" AOAHID_LIBUSB_VERSION_OK)
    if(NOT AOAHID_LIBUSB_VERSION_OK)
      message(FATAL_ERROR
          "libusb headers report LIBUSB_API_VERSION older than 0x0100010C (libusb 1.0.30).")
    endif()
    add_library(aoahid_libusb UNKNOWN IMPORTED)
    set_target_properties(aoahid_libusb PROPERTIES
        IMPORTED_LOCATION "${LIBUSB_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBUSB_INCLUDE_DIR}")
    set(AOAHID_USB_LINK_TARGET aoahid_libusb)
  endif()
endif()

# The check adds members to the internal Context, so the library and every
# test that includes its internal headers must agree on it.
if(AOAHID_CHECK_SERIALIZATION)
  add_compile_definitions(AOAHID_CHECK_SERIALIZATION=1)
endif()

# The shared and static libraries intentionally use separately compiled object
# files.  On Windows AOAHID_BUILDING_LIBRARY expands AOAHID_API to
# __declspec(dllexport), and reusing those objects in the static archive would
# leak /EXPORT directives into a consumer executable.  The test object set is
# separate as well so tests remain available in shared-only configurations
# without linking hidden internal implementation symbols from the DLL.
function(aoahid_add_objects target export_api)
  add_library(${target} OBJECT ${AOAHID_SOURCES})
  set_target_properties(${target} PROPERTIES
      CXX_STANDARD 20
      CXX_STANDARD_REQUIRED YES
      CXX_EXTENSIONS NO
      POSITION_INDEPENDENT_CODE YES)
  target_include_directories(${target}
      PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  if(export_api)
    target_compile_definitions(${target} PRIVATE AOAHID_BUILDING_LIBRARY=1)
  endif()
  if(AOAHID_USE_FAKE_LIBUSB)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_libusb)
    target_sources(${target} PRIVATE tests/fake_libusb/fake_libusb.cpp)
  elseif(AOAHID_USB_LINK_TARGET)
    # Object-library compilation itself needs libusb's usage requirements;
    # linking only the final archive/shared library is too late for its headers.
    target_link_libraries(${target} PRIVATE ${AOAHID_USB_LINK_TARGET})
  endif()

  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 $<$<BOOL:${AOAHID_WERROR}>:/WX> /permissive- /EHsc /GR- /utf-8)
  else()
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
        $<$<BOOL:${AOAHID_WERROR}>:-Werror> -fno-rtti -fvisibility=hidden)
  endif()

  if(AOAHID_INSTRUMENT_COMPILE_OPTIONS)
    target_compile_options(${target} PRIVATE
        ${AOAHID_INSTRUMENT_COMPILE_OPTIONS})
  endif()
endfunction()

function(aoahid_instrument_executable target)
  if(AOAHID_INSTRUMENT_COMPILE_OPTIONS)
    target_compile_options(${target} PRIVATE
        ${AOAHID_INSTRUMENT_COMPILE_OPTIONS})
    target_link_options(${target} PRIVATE ${AOAHID_INSTRUMENT_LINK_OPTIONS})
  endif()
endfunction()

if(AOAHID_BUILD_SHARED)
  aoahid_add_objects(aoahid_shared_objects TRUE)
endif()
if(AOAHID_BUILD_STATIC)
  aoahid_add_objects(aoahid_static_objects FALSE)
endif()
if(AOAHID_BUILD_TESTS)
  aoahid_add_objects(aoahid_test_objects FALSE)
endif()
if(AOAHID_BUILD_FUZZ AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  aoahid_add_objects(aoahid_fuzz_objects FALSE)
endif()

# The public ABI is C, but a static archive still contains C++ objects.  A
# consumer whose project enables only C must receive the C++ runtime as a
# transitive link-only dependency; selecting it from the compiler's own
# implicit-link metadata keeps this correct for GCC/libstdc++ and
# Clang/libc++ toolchains without guessing a platform runtime.
set(AOAHID_CXX_RUNTIME_LIBRARY "")
if(AOAHID_BUILD_STATIC AND NOT MSVC)
  foreach(AOAHID_CXX_RUNTIME_CANDIDATE IN ITEMS stdc++ c++)
    if(AOAHID_CXX_RUNTIME_LIBRARY STREQUAL "")
      list(FIND CMAKE_CXX_IMPLICIT_LINK_LIBRARIES
          "${AOAHID_CXX_RUNTIME_CANDIDATE}"
          AOAHID_CXX_RUNTIME_INDEX)
      if(NOT AOAHID_CXX_RUNTIME_INDEX EQUAL -1)
        set(AOAHID_CXX_RUNTIME_LIBRARY "${AOAHID_CXX_RUNTIME_CANDIDATE}")
      endif()
    endif()
  endforeach()
  if(AOAHID_CXX_RUNTIME_LIBRARY STREQUAL "")
    message(FATAL_ERROR
        "Cannot derive the C++ runtime required by static C consumers from CMAKE_CXX_IMPLICIT_LINK_LIBRARIES.")
  endif()
endif()

function(aoahid_configure_library target kind objects)
  add_library(${target} ${kind} $<TARGET_OBJECTS:${objects}>)
  if(AOAHID_USB_LINK_TARGET)
    if(kind STREQUAL "STATIC")
      target_link_libraries(${target} PUBLIC ${AOAHID_USB_LINK_TARGET})
    else()
      target_link_libraries(${target} PRIVATE ${AOAHID_USB_LINK_TARGET})
    endif()
  endif()
  if(kind STREQUAL "STATIC" AND NOT MSVC)
    target_link_libraries(${target} INTERFACE
        "$<LINK_ONLY:${AOAHID_CXX_RUNTIME_LIBRARY}>")
  endif()
  target_include_directories(${target} PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  set_target_properties(${target} PROPERTIES
      VERSION ${PROJECT_VERSION}
      SOVERSION 2
      CXX_STANDARD 20
      CXX_STANDARD_REQUIRED YES)
  if(AOAHID_INSTRUMENT_LINK_OPTIONS)
    # A consumer participating in this diagnostic build must be instrumented
    # too; sanitizer runtimes cannot observe accesses in uninstrumented code.
    # These interface requirements do not exist in ordinary release exports.
    target_compile_options(${target} INTERFACE
        ${AOAHID_INSTRUMENT_COMPILE_OPTIONS})
    if(kind STREQUAL "STATIC")
      # The archive contains instrumented objects but has no link step of its
      # own.  Propagate the sanitizer runtimes to every executable/shared
      # library that consumes this target (including the descriptor exporter).
      target_link_options(${target} INTERFACE
          ${AOAHID_INSTRUMENT_LINK_OPTIONS})
    else()
      # The shared object needs the runtime for its own link.  PUBLIC supplies
      # the same audited runtime option to instrumented consumers as well.
      target_link_options(${target} PUBLIC ${AOAHID_INSTRUMENT_LINK_OPTIONS})
    endif()
  endif()
endfunction()

if(AOAHID_BUILD_SHARED)
  aoahid_configure_library(aoahid SHARED aoahid_shared_objects)
  add_library(aoahid::shared ALIAS aoahid)
  target_compile_definitions(aoahid INTERFACE AOAHID_SHARED=1)
  if(APPLE)
    set(AOAHID_APPLE_EXPORT_LIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/aoahid.exports")
    target_link_options(aoahid PRIVATE
        "LINKER:-exported_symbols_list,${AOAHID_APPLE_EXPORT_LIST}")
    set_property(TARGET aoahid APPEND PROPERTY LINK_DEPENDS
        "${AOAHID_APPLE_EXPORT_LIST}")
  elseif(UNIX)
    set(AOAHID_VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/aoahid.map")
    # The ABI is plain C, so the C++ runtime stays private to the shared
    # object: no libstdc++ or libgcc_s dependency, no runtime symbols exported.
    target_link_options(aoahid PRIVATE
        -static-libstdc++ -static-libgcc "-Wl,--exclude-libs,ALL"
        "-Wl,--version-script=${AOAHID_VERSION_SCRIPT}")
    set_property(TARGET aoahid APPEND PROPERTY LINK_DEPENDS "${AOAHID_VERSION_SCRIPT}")
  endif()
endif()

if(AOAHID_BUILD_STATIC)
  aoahid_configure_library(aoahid_static STATIC aoahid_static_objects)
  add_library(aoahid::static ALIAS aoahid_static)
  if(WIN32 OR AOAHID_BUILD_SHARED)
    set_target_properties(aoahid_static PROPERTIES OUTPUT_NAME aoahid_static)
  else()
    set_target_properties(aoahid_static PROPERTIES OUTPUT_NAME aoahid)
  endif()
  # Keep the installed static target name invariant across static-only and
  # combined packages.  The package config supplies the historical primary
  # name as an alias when no shared target is present.
  set_target_properties(aoahid_static PROPERTIES EXPORT_NAME aoahid_static)
endif()

set(AOAHID_PRIMARY_TARGET aoahid_static)
if(AOAHID_BUILD_SHARED)
  set(AOAHID_PRIMARY_TARGET aoahid)
endif()
add_library(aoahid::aoahid ALIAS ${AOAHID_PRIMARY_TARGET})

# The stable C ABI remains usable from a project that enables only C.  The
# separate C++ facade carries the language-level requirement of aoahid.hpp
# without making such pure-C consumers enable CXX merely to link the library.
if(AOAHID_BUILD_SHARED)
  add_library(aoahid_cpp INTERFACE)
  add_library(aoahid::cpp ALIAS aoahid_cpp)
  target_link_libraries(aoahid_cpp INTERFACE aoahid)
  target_compile_features(aoahid_cpp INTERFACE cxx_std_20)
  set_target_properties(aoahid_cpp PROPERTIES EXPORT_NAME cpp)
endif()
if(AOAHID_BUILD_STATIC)
  if(AOAHID_BUILD_SHARED)
    add_library(aoahid_cpp_static INTERFACE)
    add_library(aoahid::cpp_static ALIAS aoahid_cpp_static)
    target_link_libraries(aoahid_cpp_static INTERFACE aoahid_static)
    target_compile_features(aoahid_cpp_static INTERFACE cxx_std_20)
    set_target_properties(aoahid_cpp_static PROPERTIES EXPORT_NAME cpp_static)
  else()
    add_library(aoahid_cpp INTERFACE)
    add_library(aoahid::cpp ALIAS aoahid_cpp)
    target_link_libraries(aoahid_cpp INTERFACE aoahid_static)
    target_compile_features(aoahid_cpp INTERFACE cxx_std_20)
    set_target_properties(aoahid_cpp PROPERTIES EXPORT_NAME cpp)
  endif()
endif()

if(AOAHID_BUILD_TESTS)
  enable_testing()
  find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
  if(WIN32)
    set(AOAHID_TEST_LIBUSB_LIBRARY_NAME libusb-1.0.lib)
  else()
    set(AOAHID_TEST_LIBUSB_LIBRARY_NAME libusb-1.0.a)
  endif()
  add_test(
      NAME aoahid.reject-old-libusb-fallback
      COMMAND ${CMAKE_COMMAND}
          -DAOAHID_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
          -DAOAHID_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/old-libusb-negative
          -DAOAHID_GENERATOR=${CMAKE_GENERATOR}
          -DAOAHID_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
          -DAOAHID_LIBRARY_NAME=${AOAHID_TEST_LIBUSB_LIBRARY_NAME}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/reject_old_libusb.cmake)
  add_test(
      NAME aoahid.package-multiarch-layout
      COMMAND ${CMAKE_COMMAND}
          -DAOAHID_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
          -DAOAHID_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/package-multiarch-layout
          -DAOAHID_GENERATOR=${CMAKE_GENERATOR}
          -DAOAHID_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
          -DAOAHID_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/package_multiarch_layout.cmake)
  add_test(
      NAME aoahid.package-component-contract
      COMMAND ${CMAKE_COMMAND}
          -DAOAHID_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
          -DAOAHID_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/package-component-contract
          -DAOAHID_GENERATOR=${CMAKE_GENERATOR}
          -DAOAHID_PROJECT_VERSION=${PROJECT_VERSION}
          -DAOAHID_PROJECT_HOMEPAGE_URL=${PROJECT_HOMEPAGE_URL}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/package_component_contract.cmake)
  if(WIN32)
    add_executable(aoahid_windows_libusb_minmax_compile
        tests/cmake/windows_libusb_minmax.cpp)
    target_include_directories(aoahid_windows_libusb_minmax_compile PRIVATE
        tests/cmake/windows_libusb_minmax_fixture
        src)
    set_target_properties(aoahid_windows_libusb_minmax_compile PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO)
    if(MSVC)
      target_compile_options(aoahid_windows_libusb_minmax_compile PRIVATE
          /W4 $<$<BOOL:${AOAHID_WERROR}>:/WX> /permissive- /EHsc /GR- /utf-8 /UNOMINMAX)
    else()
      target_compile_options(aoahid_windows_libusb_minmax_compile PRIVATE
          -Wall -Wextra -Wpedantic $<$<BOOL:${AOAHID_WERROR}>:-Werror> -fno-rtti -UNOMINMAX)
    endif()
    add_test(NAME aoahid.windows-libusb-minmax
        COMMAND aoahid_windows_libusb_minmax_compile)
  endif()
  add_executable(aoahid_abi_layout_oracle tests/abi/layout_oracle.c)
  target_include_directories(aoahid_abi_layout_oracle PRIVATE include)
  set_target_properties(aoahid_abi_layout_oracle PROPERTIES
      C_STANDARD 11
      C_STANDARD_REQUIRED YES
      C_EXTENSIONS NO)
  if(MSVC)
    target_compile_options(aoahid_abi_layout_oracle PRIVATE /W4 $<$<BOOL:${AOAHID_WERROR}>:/WX> /utf-8)
  else()
    target_compile_options(aoahid_abi_layout_oracle PRIVATE
        -Wall -Wextra -Wpedantic $<$<BOOL:${AOAHID_WERROR}>:-Werror> -fshort-enums)
  endif()
  aoahid_instrument_executable(aoahid_abi_layout_oracle)
  add_test(
      NAME aoahid.abi-layout-oracle
      COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/abi/check_abi_golden.py
          $<TARGET_FILE:aoahid_abi_layout_oracle>)

  add_executable(aoahid_c_abi_test tests/abi/test_c_abi.c)
  target_include_directories(aoahid_c_abi_test PRIVATE include)
  set_target_properties(aoahid_c_abi_test PROPERTIES
      C_STANDARD 11
      C_STANDARD_REQUIRED YES
      C_EXTENSIONS NO)
  if(MSVC)
    target_compile_options(aoahid_c_abi_test PRIVATE /W4 $<$<BOOL:${AOAHID_WERROR}>:/WX> /utf-8)
  else()
    target_compile_options(aoahid_c_abi_test PRIVATE
        -Wall -Wextra -Wpedantic $<$<BOOL:${AOAHID_WERROR}>:-Werror> -fshort-enums)
  endif()
  aoahid_instrument_executable(aoahid_c_abi_test)
  add_test(NAME aoahid.c-abi COMMAND aoahid_c_abi_test)

  add_executable(aoahid_cpp_wrapper_test tests/abi/test_cpp_wrapper.cpp)
  target_link_libraries(aoahid_cpp_wrapper_test PRIVATE aoahid::aoahid)
  set_target_properties(aoahid_cpp_wrapper_test PROPERTIES
      CXX_STANDARD 20
      CXX_STANDARD_REQUIRED YES
      CXX_EXTENSIONS NO)
  if(MSVC)
    target_compile_options(aoahid_cpp_wrapper_test PRIVATE
        /W4 $<$<BOOL:${AOAHID_WERROR}>:/WX> /permissive- /EHsc /GR- /utf-8)
  else()
    target_compile_options(aoahid_cpp_wrapper_test PRIVATE
        -Wall -Wextra -Wpedantic $<$<BOOL:${AOAHID_WERROR}>:-Werror> -fno-rtti)
  endif()
  aoahid_instrument_executable(aoahid_cpp_wrapper_test)
  add_test(NAME aoahid.cpp-wrapper COMMAND aoahid_cpp_wrapper_test)

  add_executable(aoahid_tests
      tests/unit/test_main.cpp
      tests/unit/test_item_writer.cpp
      tests/unit/test_profiles.cpp
      tests/integration/test_transport.cpp
      tests/integration/test_identity.cpp
      $<TARGET_OBJECTS:aoahid_test_objects>)
  target_include_directories(aoahid_tests PRIVATE include src tests tests/fake_libusb)
  set_target_properties(aoahid_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
  if(MSVC)
    target_compile_options(aoahid_tests PRIVATE /W4 $<$<BOOL:${AOAHID_WERROR}>:/WX> /EHsc /GR-)
  else()
    target_compile_options(aoahid_tests PRIVATE -Wall -Wextra -Wpedantic $<$<BOOL:${AOAHID_WERROR}>:-Werror> -fno-rtti)
  endif()
  aoahid_instrument_executable(aoahid_tests)
  add_test(NAME aoahid.unit-and-integration COMMAND aoahid_tests)

  add_executable(aoahid_internal_thread_test
      tests/integration/test_internal_thread.cpp
      $<TARGET_OBJECTS:aoahid_test_objects>)
  target_include_directories(aoahid_internal_thread_test
      PRIVATE include src tests tests/fake_libusb)
  set_target_properties(aoahid_internal_thread_test PROPERTIES
      CXX_STANDARD 20
      CXX_STANDARD_REQUIRED YES
      CXX_EXTENSIONS NO)
  if(MSVC)
    target_compile_options(aoahid_internal_thread_test PRIVATE
        /W4 $<$<BOOL:${AOAHID_WERROR}>:/WX> /permissive- /EHsc /GR- /utf-8)
  else()
    target_compile_options(aoahid_internal_thread_test PRIVATE
        -Wall -Wextra -Wpedantic $<$<BOOL:${AOAHID_WERROR}>:-Werror> -fno-rtti)
  endif()
  aoahid_instrument_executable(aoahid_internal_thread_test)
  add_test(NAME aoahid.internal-thread COMMAND aoahid_internal_thread_test)

  if(AOAHID_CHECK_SERIALIZATION)
    add_executable(aoahid_serialization_check_test
        tests/integration/test_serialization_check.cpp
        $<TARGET_OBJECTS:aoahid_test_objects>)
    target_include_directories(aoahid_serialization_check_test
        PRIVATE include src tests tests/fake_libusb)
    set_target_properties(aoahid_serialization_check_test PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO)
    if(MSVC)
      target_compile_options(aoahid_serialization_check_test PRIVATE
          /W4 $<$<BOOL:${AOAHID_WERROR}>:/WX> /permissive- /EHsc /GR- /utf-8)
    else()
      target_compile_options(aoahid_serialization_check_test PRIVATE
          -Wall -Wextra -Wpedantic $<$<BOOL:${AOAHID_WERROR}>:-Werror> -fno-rtti)
    endif()
    aoahid_instrument_executable(aoahid_serialization_check_test)
    add_test(NAME aoahid.serialization-check-ok
        COMMAND aoahid_serialization_check_test ok)
    # A debug C runtime on Windows answers abort() with a dialog and waits.
    if(NOT WIN32)
      add_test(
          NAME aoahid.serialization-check-violation
          COMMAND ${CMAKE_COMMAND}
              -DAOAHID_SERIALIZATION_TEST=$<TARGET_FILE:aoahid_serialization_check_test>
              -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/expect_serialization_abort.cmake)
    endif()
  endif()

  add_executable(aoahid_export_descriptors tests/hidtools/export_descriptors.cpp)
  target_link_libraries(aoahid_export_descriptors PRIVATE aoahid::aoahid)
  target_include_directories(aoahid_export_descriptors PRIVATE src)
  set_target_properties(aoahid_export_descriptors PROPERTIES
      CXX_STANDARD 20
      CXX_STANDARD_REQUIRED YES
      CXX_EXTENSIONS NO)
  if(MSVC)
    target_compile_options(aoahid_export_descriptors PRIVATE
        /W4 $<$<BOOL:${AOAHID_WERROR}>:/WX> /permissive- /EHsc /GR- /utf-8)
  else()
    target_compile_options(aoahid_export_descriptors PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
        $<$<BOOL:${AOAHID_WERROR}>:-Werror> -fno-rtti)
  endif()
  aoahid_instrument_executable(aoahid_export_descriptors)
  add_test(
      NAME aoahid.golden-descriptors
      COMMAND aoahid_export_descriptors
          ${CMAKE_CURRENT_BINARY_DIR}/golden-descriptor-output
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden
          ${CMAKE_CURRENT_BINARY_DIR}/support-manifests.tsv)
  add_test(
      NAME aoahid.support-table
      COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-support-table/generate.py
          --check
          --readme ${CMAKE_CURRENT_SOURCE_DIR}/README.md
          --catalog ${CMAKE_CURRENT_BINARY_DIR}/support-manifests.tsv)
  set_tests_properties(aoahid.support-table PROPERTIES
      DEPENDS aoahid.golden-descriptors)
endif()

if(AOAHID_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(AOAHID_BUILD_FUZZ AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_executable(aoahid_fuzz_descriptor
      tests/fuzz/fuzz_descriptor.cpp
      $<TARGET_OBJECTS:aoahid_fuzz_objects>)
  set_target_properties(aoahid_fuzz_descriptor PROPERTIES
      CXX_STANDARD 20
      CXX_STANDARD_REQUIRED YES
      CXX_EXTENSIONS NO)
  target_include_directories(aoahid_fuzz_descriptor PRIVATE include src tests/fake_libusb)
  if(AOAHID_USB_LINK_TARGET)
    target_link_libraries(aoahid_fuzz_descriptor PRIVATE ${AOAHID_USB_LINK_TARGET})
  endif()
  target_compile_options(aoahid_fuzz_descriptor PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(aoahid_fuzz_descriptor PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

set(AOAHID_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/aoahid")
set(AOAHID_PC_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

if(AOAHID_BUILD_SHARED)
  install(TARGETS aoahid aoahid_cpp
      EXPORT aoahid-shared-targets
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT aoahid-shared-targets
      FILE aoahid-shared-targets.cmake
      NAMESPACE aoahid::
      DESTINATION ${AOAHID_CMAKE_INSTALL_DIR})
endif()
if(AOAHID_BUILD_STATIC)
  set(AOAHID_STATIC_INSTALL_TARGETS aoahid_static)
  if(AOAHID_BUILD_SHARED)
    list(APPEND AOAHID_STATIC_INSTALL_TARGETS aoahid_cpp_static)
  else()
    list(APPEND AOAHID_STATIC_INSTALL_TARGETS aoahid_cpp)
  endif()
  install(TARGETS ${AOAHID_STATIC_INSTALL_TARGETS}
      EXPORT aoahid-static-targets
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT aoahid-static-targets
      FILE aoahid-static-targets.cmake
      NAMESPACE aoahid::
      DESTINATION ${AOAHID_CMAKE_INSTALL_DIR})
endif()

install(FILES include/aoahid.h include/aoahid.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES cmake/aoahid-check-libusb.cmake
    DESTINATION ${AOAHID_CMAKE_INSTALL_DIR})

# These carry the MIT notice and, through libusb-1.0 (LGPL-2.1-or-later), the
# third-party notices a redistributor is required to pass along. The official
# release archives already include them (see tools/release/); this makes a
# plain `cmake --install` from source carry them too.
install(FILES LICENSE NOTICE THIRD_PARTY_NOTICES.md
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/libaoahid)

configure_package_config_file(
    cmake/aoahid-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/aoahid-config.cmake
    INSTALL_DESTINATION ${AOAHID_CMAKE_INSTALL_DIR}
    PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR)
configure_file(
    cmake/aoahid-config-version.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/aoahid-config-version.cmake
    @ONLY)

# Derive the pkg-config prefix from the actual pkgconfig directory depth.  The
# previous fixed ../.. was correct only for lib/pkgconfig and produced a prefix
# inside lib/ for GNU multiarch layouts such as lib/<tuple>/pkgconfig.
if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
  set(AOAHID_PC_PREFIX "${CMAKE_INSTALL_PREFIX}")
  set(AOAHID_PC_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
else()
  file(RELATIVE_PATH AOAHID_PC_PREFIX_FROM_PCDIR
      "${CMAKE_INSTALL_PREFIX}/${AOAHID_PC_INSTALL_DIR}" "${CMAKE_INSTALL_PREFIX}")
  string(REGEX REPLACE "/+$" "" AOAHID_PC_PREFIX_FROM_PCDIR
      "${AOAHID_PC_PREFIX_FROM_PCDIR}")
  set(AOAHID_PC_PREFIX
      "\${pcfiledir}/${AOAHID_PC_PREFIX_FROM_PCDIR}")
  set(AOAHID_PC_LIBDIR "\${exec_prefix}/${CMAKE_INSTALL_LIBDIR}")
endif()
if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
  set(AOAHID_PC_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
else()
  set(AOAHID_PC_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
endif()

set(AOAHID_PC_STATIC_REQUIRES_PRIVATE "")
set(AOAHID_PC_STATIC_LIBS_PRIVATE "")
if(AOAHID_BUILD_STATIC AND NOT AOAHID_USE_FAKE_LIBUSB)
  set(AOAHID_PC_STATIC_REQUIRES_PRIVATE
      "Requires.private: libusb-1.0 >= 1.0.30")
endif()
if(AOAHID_BUILD_STATIC)
  if(NOT MSVC)
    set(AOAHID_PC_STATIC_LIBS_PRIVATE
        "Libs.private: -l${AOAHID_CXX_RUNTIME_LIBRARY}")
  endif()
endif()

set(AOAHID_PC_FILES "")
if(AOAHID_BUILD_SHARED)
  set(AOAHID_PC_NAME "libaoahid")
  set(AOAHID_PC_LIBRARY_NAME aoahid)
  set(AOAHID_PC_REQUIRES_PRIVATE "")
  set(AOAHID_PC_LIBS_PRIVATE "")
  configure_file(cmake/aoahid.pc.in ${CMAKE_CURRENT_BINARY_DIR}/aoahid.pc @ONLY)
  list(APPEND AOAHID_PC_FILES ${CMAKE_CURRENT_BINARY_DIR}/aoahid.pc)
endif()
if(AOAHID_BUILD_STATIC)
  set(AOAHID_PC_NAME "libaoahid")
  set(AOAHID_PC_LIBRARY_NAME aoahid)
  if(WIN32 OR AOAHID_BUILD_SHARED)
    set(AOAHID_PC_LIBRARY_NAME aoahid_static)
  endif()
  set(AOAHID_PC_REQUIRES_PRIVATE "${AOAHID_PC_STATIC_REQUIRES_PRIVATE}")
  set(AOAHID_PC_LIBS_PRIVATE "${AOAHID_PC_STATIC_LIBS_PRIVATE}")
  if(AOAHID_BUILD_SHARED)
    set(AOAHID_PC_NAME "libaoahid static")
    configure_file(
        cmake/aoahid.pc.in
        ${CMAKE_CURRENT_BINARY_DIR}/aoahid-static.pc
        @ONLY)
    list(APPEND AOAHID_PC_FILES ${CMAKE_CURRENT_BINARY_DIR}/aoahid-static.pc)
  else()
    configure_file(cmake/aoahid.pc.in ${CMAKE_CURRENT_BINARY_DIR}/aoahid.pc @ONLY)
    list(APPEND AOAHID_PC_FILES ${CMAKE_CURRENT_BINARY_DIR}/aoahid.pc)
  endif()
endif()
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/aoahid-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/aoahid-config-version.cmake
    DESTINATION ${AOAHID_CMAKE_INSTALL_DIR})
install(FILES ${AOAHID_PC_FILES} DESTINATION ${AOAHID_PC_INSTALL_DIR})
