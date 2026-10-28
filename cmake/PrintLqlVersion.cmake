include("${CMAKE_CURRENT_LIST_DIR}/LqlVersion.cmake")
if(DEFINED LQL_VERSION_HEADER)
  configure_file("${CMAKE_CURRENT_LIST_DIR}/lql_version.h.in"
    "${LQL_VERSION_HEADER}" @ONLY)
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${LQL_RESOLVED_VERSION}")
