include("${CMAKE_CURRENT_LIST_DIR}/cpkt-toolchain.cmake")

if(NOT CPKT_TARGET_ID STREQUAL "x86_64-linux-gnu")
  message(FATAL_ERROR "AFL++ fuzzing is native x86_64-linux-gnu only")
endif()

set(CPKT_AFLPP_RESOLVER "${CMAKE_CURRENT_LIST_DIR}/../scripts/cpkt-aflpp.sh")
if(NOT EXISTS "${CPKT_AFLPP_RESOLVER}")
  message(FATAL_ERROR "Missing AFL++ resolver: ${CPKT_AFLPP_RESOLVER}")
endif()

execute_process(
  COMMAND "${CPKT_AFLPP_RESOLVER}" discover
  RESULT_VARIABLE CPKT_AFL_RESULT
  OUTPUT_VARIABLE CPKT_AFL_OUTPUT
  ERROR_VARIABLE CPKT_AFL_ERROR)
if(NOT CPKT_AFL_RESULT EQUAL 0)
  message(FATAL_ERROR "Unable to provision pinned AFL++: ${CPKT_AFL_ERROR}")
endif()

function(cpkt_afl_value name out)
  string(REGEX MATCH "(^|\n)${name}=([^\n]+)" match "${CPKT_AFL_OUTPUT}")
  if(NOT match)
    message(FATAL_ERROR "AFL++ resolver did not report ${name}")
  endif()
  set(${out} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

cpkt_afl_value(cc CPKT_AFL_CC)
cpkt_afl_value(cxx CPKT_AFL_CXX)
cpkt_afl_value(helper CPKT_AFL_HELPER)

set(ENV{AFL_PATH} "${CPKT_AFL_HELPER}")
set(CMAKE_C_COMPILER "${CPKT_AFL_CC}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${CPKT_AFL_CXX}" CACHE FILEPATH "" FORCE)
