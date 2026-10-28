cmake_minimum_required(VERSION 3.20)
project(liblql_lifecycle_checks NONE)
enable_testing()
get_filename_component(root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(scratch "${root}/build/tmp")
file(MAKE_DIRECTORY "${scratch}")
find_program(HOST_BASH bash REQUIRED)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME lifecycle.toolchains
    COMMAND "${HOST_BASH}" "${root}/scripts/test-cpkt-toolchain-resolvers.sh")
  set_tests_properties(lifecycle.toolchains PROPERTIES
    TIMEOUT 30 RUN_SERIAL TRUE LABELS "resolver;local"
    ENVIRONMENT "TMPDIR=${scratch}")
  if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    add_test(NAME lifecycle.afl
      COMMAND "${HOST_BASH}" "${root}/scripts/test-cpkt-aflpp-resolver.sh")
    set_tests_properties(lifecycle.afl PROPERTIES
      TIMEOUT 30 RUN_SERIAL TRUE LABELS "resolver;local"
      ENVIRONMENT "TMPDIR=${scratch}")
  endif()
endif()
add_test(NAME lifecycle.integration
  COMMAND "${HOST_BASH}" "${root}/tests/lifecycle/integration.sh")
set_tests_properties(lifecycle.integration PROPERTIES
  TIMEOUT 30 RUN_SERIAL TRUE LABELS "integration;local"
  ENVIRONMENT "TMPDIR=${scratch}")
foreach(check dependency_cache tar contracts feedback darwin_config)
  if(check STREQUAL "dependency_cache")
    set(script "${root}/scripts/test_dependency_cache_config.sh")
  elseif(check STREQUAL "tar")
    set(script "${root}/scripts/check_tar_portability.sh")
  elseif(check STREQUAL "contracts")
    set(script "${CMAKE_CURRENT_SOURCE_DIR}/contracts.sh")
  elseif(check STREQUAL "feedback")
    set(script "${CMAKE_CURRENT_SOURCE_DIR}/feedback.sh")
  else()
    set(script "${CMAKE_CURRENT_SOURCE_DIR}/darwin-config.sh")
  endif()
  add_test(NAME lifecycle.${check} COMMAND "${HOST_BASH}" "${script}")
  set_tests_properties(lifecycle.${check} PROPERTIES
    TIMEOUT 60 RUN_SERIAL TRUE LABELS "integration;local"
    WORKING_DIRECTORY "${root}" ENVIRONMENT "TMPDIR=${scratch}")
endforeach()
foreach(mode local local-spaces global mixed explicit-only)
  add_test(NAME lifecycle.version_cache.${mode}
    COMMAND "${HOST_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/version-cache.sh" "${mode}")
  set_tests_properties(lifecycle.version_cache.${mode} PROPERTIES
    TIMEOUT 60 RUN_SERIAL TRUE LABELS "integration;local"
    ENVIRONMENT "TMPDIR=${scratch}")
endforeach()
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME lifecycle.qemu
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/qemu.cmake")
  set_tests_properties(lifecycle.qemu PROPERTIES
    TIMEOUT 30 RUN_SERIAL TRUE LABELS "integration;local")
endif()
