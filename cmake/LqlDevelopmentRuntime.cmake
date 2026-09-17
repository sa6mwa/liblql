# Development executables run against the exact Bootlin libc selected at
# configure time.  This is deliberately private to non-shipped executables:
# SDK libraries and release binaries must never embed a workstation cache path.
function(lql_configure_development_runtime target)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CPKT_SOURCE STREQUAL "bootlin")
    return()
  endif()
  if(NOT DEFINED CPKT_DYNAMIC_LOADER OR CPKT_DYNAMIC_LOADER STREQUAL "" OR
     NOT DEFINED CPKT_RUNTIME_LIBRARY_DIRS OR CPKT_RUNTIME_LIBRARY_DIRS STREQUAL "")
    message(FATAL_ERROR
      "Bootlin development runtime metadata is unavailable for ${target}")
  endif()
  set(runtime_dirs ${CPKT_RUNTIME_LIBRARY_DIRS} ${ARGN})
  list(REMOVE_DUPLICATES runtime_dirs)
  target_link_options(${target} PRIVATE
    "-Wl,--dynamic-linker=${CPKT_DYNAMIC_LOADER}")
  foreach(runtime_dir IN LISTS runtime_dirs)
    if(runtime_dir STREQUAL "")
      continue()
    endif()
    target_link_options(${target} PRIVATE
      "-Wl,--disable-new-dtags"
      "-Wl,-rpath,${runtime_dir}")
  endforeach()
endfunction()
