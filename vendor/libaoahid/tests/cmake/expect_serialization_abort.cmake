# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
if(NOT DEFINED AOAHID_SERIALIZATION_TEST)
  message(FATAL_ERROR "serialization check test inputs are incomplete")
endif()

# The check ends the process with abort(), which CTest counts as a failed
# test whatever it printed; so the program is run from here and both its end
# and its message are examined.
execute_process(
    COMMAND "${AOAHID_SERIALIZATION_TEST}" violation
    RESULT_VARIABLE result
    OUTPUT_QUIET
    ERROR_VARIABLE messages)

if(result EQUAL 0 OR messages MATCHES "the call returned")
  message(FATAL_ERROR "a second thread entered the Context unnoticed: ${messages}")
endif()
if(NOT messages MATCHES
   "aoahid_context_poll was called while another thread is inside a call on the same Context")
  message(FATAL_ERROR "the process ended (${result}) without naming the call: ${messages}")
endif()
