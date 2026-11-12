cmake_minimum_required(VERSION 3.20)
project(cordgeist_discovery_fake_abi LANGUAGES C CXX)
# TEST ONLY. Never install or package this library; real native builds use the
# unmodified upstream configuration in build/native-real and native-prefix.
set(AOAHID_USE_FAKE_LIBUSB ON CACHE BOOL "" FORCE)
set(AOAHID_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AOAHID_BUILD_STATIC OFF CACHE BOOL "" FORCE)
set(AOAHID_BUILD_SHARED ON CACHE BOOL "" FORCE)
add_subdirectory(../../vendor/libaoahid upstream)
target_sources(aoahid_shared_objects PRIVATE fixture.cpp)
# Expose only in this isolated test build the fixture marker/control functions.
# Upstream production symbol export/visibility settings remain unchanged.
target_compile_options(aoahid_shared_objects PRIVATE -fvisibility=default)
# Preserve upstream runtime/sanitizer link flags; replace only its export map.
get_target_property(_aoahid_test_link_options aoahid LINK_OPTIONS)
list(FILTER _aoahid_test_link_options EXCLUDE REGEX ".*--version-script=.*")
set_property(TARGET aoahid PROPERTY LINK_OPTIONS "${_aoahid_test_link_options}")
target_link_options(aoahid PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/fake-test.map")
set_property(TARGET aoahid APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fake-test.map")
