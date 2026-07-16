function(lql_resolve_version out_var)
  execute_process(
    COMMAND git tag --points-at HEAD --list "v[0-9]*.[0-9]*.[0-9]*"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE tags
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(resolved_tag_version "")
  if(tags)
    string(REPLACE "\n" ";" tag_list "${tags}")
    foreach(candidate IN LISTS tag_list)
      if(candidate MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)$")
        set(candidate_version "${CMAKE_MATCH_1}")
        execute_process(
          COMMAND git cat-file -t "refs/tags/${candidate}"
          WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
          OUTPUT_VARIABLE candidate_type
          ERROR_QUIET
          OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(candidate_type STREQUAL "commit")
          if(resolved_tag_version AND
             NOT resolved_tag_version STREQUAL candidate_version)
            message(FATAL_ERROR
              "Multiple lightweight release tags point at HEAD: ${resolved_tag_version} and ${candidate_version}")
          endif()
          set(resolved_tag_version "${candidate_version}")
        endif()
      endif()
    endforeach()
  endif()
  if(resolved_tag_version)
    set(${out_var} "${resolved_tag_version}" PARENT_SCOPE)
  elseif(LQL_VERSION_OVERRIDE)
    set(${out_var} "${LQL_VERSION_OVERRIDE}" PARENT_SCOPE)
  elseif(DEFINED ENV{LQL_VERSION_OVERRIDE} AND NOT "$ENV{LQL_VERSION_OVERRIDE}" STREQUAL "")
    set(${out_var} "$ENV{LQL_VERSION_OVERRIDE}" PARENT_SCOPE)
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
if(LQL_RESOLVED_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
  set(LQL_VERSION_MAJOR "${CMAKE_MATCH_1}")
  set(LQL_VERSION_MINOR "${CMAKE_MATCH_2}")
  set(LQL_VERSION_PATCH "${CMAKE_MATCH_3}")
else()
  message(FATAL_ERROR "Resolved liblql version is not semver: ${LQL_RESOLVED_VERSION}")
endif()
