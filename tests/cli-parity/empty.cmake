execute_process(COMMAND "${BINARY}" -test.run "^NoSuchTest$"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 1 OR NOT error MATCHES "empty parity test selection")
  message(FATAL_ERROR "Expected empty-selection rejection: ${result}\n${output}\n${error}")
endif()
