# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
cmake_minimum_required(VERSION 3.20)
if(NOT PROJECT_NAME)
  # Standalone: build the examples against an installed package.
  project(aoahid_examples LANGUAGES C CXX)
  set(CMAKE_C_STANDARD 11)
  set(CMAKE_CXX_STANDARD 20)
  find_package(aoahid REQUIRED)
endif()

# Inside the libaoahid build the executables carry the same sanitizer instrumentation as the
# library and sit next to its DLL; against an installed package there is nothing to do.
function(aoahid_example_instrument target)
  if(COMMAND aoahid_instrument_executable)
    aoahid_instrument_executable(${target})
  endif()
  # Windows finds aoahid.dll only next to the executable, and CTest runs the examples from the
  # build tree: build them where the library is.
  if(WIN32 AND TARGET aoahid)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:aoahid>")
  endif()
endfunction()

if(NOT WIN32)
  find_package(Threads REQUIRED)
endif()

add_executable(aoahid_example_c c/multi_profile.c)
target_link_libraries(aoahid_example_c PRIVATE aoahid::aoahid)
if(NOT WIN32)
  target_link_libraries(aoahid_example_c PRIVATE Threads::Threads)
endif()
aoahid_example_instrument(aoahid_example_c)

add_executable(aoahid_example_cpp cpp/multitouch_gesture.cpp)
target_link_libraries(aoahid_example_cpp PRIVATE aoahid::cpp)
set_target_properties(aoahid_example_cpp PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO)
aoahid_example_instrument(aoahid_example_cpp)

# One example per aoahid_profile_kind value. Each builds a complete Spec and
# needs no device, so every one of them is runnable in CI.
set(AOAHID_PROFILE_EXAMPLES
    keyboard
    mouse
    toggle
    gamepad
    touchscreen
    touchpad
    pen
    digitizer
    keyboard_battery
    raw)
foreach(aoahid_profile_example IN LISTS AOAHID_PROFILE_EXAMPLES)
  add_executable(aoahid_example_${aoahid_profile_example}
      c/profiles/${aoahid_profile_example}.c)
  target_link_libraries(aoahid_example_${aoahid_profile_example} PRIVATE aoahid::aoahid)
  set_target_properties(aoahid_example_${aoahid_profile_example} PROPERTIES
      C_STANDARD 11
      C_STANDARD_REQUIRED YES
      C_EXTENSIONS NO)
  aoahid_example_instrument(aoahid_example_${aoahid_profile_example})
  if(AOAHID_BUILD_TESTS)
    string(REPLACE "_" "-" aoahid_profile_test_name "${aoahid_profile_example}")
    add_test(NAME aoahid.example-profile-${aoahid_profile_test_name}
        COMMAND aoahid_example_${aoahid_profile_example})
  endif()
endforeach()

if(AOAHID_BUILD_TESTS)
  add_test(NAME aoahid.example-c-shared COMMAND aoahid_example_c shared)
  add_test(NAME aoahid.example-c-threaded COMMAND aoahid_example_c threaded)
endif()

# Human-observable real-device verification programs (docs/QUICKSTART.md).
# These need an actual phone and are meant to be watched, so unlike the
# examples above they are never registered as ctest cases.
set(AOAHID_VERIFY_EXAMPLES keyboard touch mouse toggle all accessory)
foreach(aoahid_verify_example IN LISTS AOAHID_VERIFY_EXAMPLES)
  add_executable(aoahid_verify_${aoahid_verify_example}
      c/verify/verify_${aoahid_verify_example}.c)
  target_link_libraries(aoahid_verify_${aoahid_verify_example} PRIVATE aoahid::aoahid)
  target_include_directories(aoahid_verify_${aoahid_verify_example} PRIVATE c/verify)
  if(NOT WIN32)
    target_link_libraries(aoahid_verify_${aoahid_verify_example} PRIVATE Threads::Threads m)
  endif()
  set_target_properties(aoahid_verify_${aoahid_verify_example} PROPERTIES
      C_STANDARD 17
      C_STANDARD_REQUIRED YES
      C_EXTENSIONS NO)
  aoahid_example_instrument(aoahid_verify_${aoahid_verify_example})
endforeach()
