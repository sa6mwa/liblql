function(lql_resolve_version out_var)
  if(LQL_VERSION_OVERRIDE)
    set(${out_var} "${LQL_VERSION_OVERRIDE}" PARENT_SCOPE)
    return()
  endif()
  execute_process(
    COMMAND git describe --tags --exact-match --match "v[0-9]*.[0-9]*.[0-9]*"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE tag
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(tag MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)$")
    set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
  elseif(EXISTS "${CMAKE_SOURCE_DIR}/VERSION" AND NOT EXISTS "${CMAKE_SOURCE_DIR}/.git")
    file(READ "${CMAKE_SOURCE_DIR}/VERSION" version_file)
    string(STRIP "${version_file}" version_file)
    set(${out_var} "${version_file}" PARENT_SCOPE)
  else()
    set(${out_var} "0.0.0" PARENT_SCOPE)
  endif()
endfunction()

lql_resolve_version(LQL_RESOLVED_VERSION)
set(PROJECT_VERSION "${LQL_RESOLVED_VERSION}")
