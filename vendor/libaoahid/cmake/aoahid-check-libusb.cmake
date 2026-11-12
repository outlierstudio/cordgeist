# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
function(aoahid_check_libusb_api_version header output_variable)
  set(_AOAHID_LIBUSB_VERSION_OK FALSE)
  if(EXISTS "${header}")
    file(STRINGS "${header}"
        _AOAHID_LIBUSB_API_LINE
        REGEX "^[ \t]*#define[ \t]+LIBUSB_API_VERSION[ \t]+0x[0-9A-Fa-f]+"
        LIMIT_COUNT 1)
    string(REGEX MATCH "0x[0-9A-Fa-f]+"
        _AOAHID_LIBUSB_API_HEX "${_AOAHID_LIBUSB_API_LINE}")
    if(_AOAHID_LIBUSB_API_HEX)
      math(EXPR _AOAHID_LIBUSB_API_NUMBER "${_AOAHID_LIBUSB_API_HEX}")
      if(_AOAHID_LIBUSB_API_NUMBER GREATER_EQUAL 16777484)
        set(_AOAHID_LIBUSB_VERSION_OK TRUE)
      endif()
    endif()
  endif()
  set("${output_variable}" "${_AOAHID_LIBUSB_VERSION_OK}" PARENT_SCOPE)
endfunction()
