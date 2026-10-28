cmake_minimum_required(VERSION 3.20)
project(liblql_qemu_smoke NONE)
enable_testing()
get_filename_component(root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

if(LQL_TARGET_ID MATCHES "^aarch64-linux-(gnu|musl)$")
  set(runner_name qemu-aarch64)
elseif(LQL_TARGET_ID MATCHES "^armhf-linux-(gnu|musl)$")
  set(runner_name qemu-arm)
else()
  message(FATAL_ERROR "QEMU smoke requires a declared ARM Linux target")
endif()
find_program(LQL_QEMU_RUNNER NAMES "${runner_name}" REQUIRED)
if(NOT EXISTS "${LQL_QEMU_RUNNER}" OR IS_DIRECTORY "${LQL_QEMU_RUNNER}")
  message(FATAL_ERROR "QEMU runner missing: ${LQL_QEMU_RUNNER}; prepare ${runner_name}")
endif()
if(NOT CPKT_SOURCE STREQUAL "bootlin" OR NOT IS_DIRECTORY "${CPKT_SYSROOT}")
  message(FATAL_ERROR "QEMU smoke requires the selected Bootlin sysroot: ${CPKT_SYSROOT}")
endif()
if(NOT LQL_QEMU_SDK_PREFIX AND NOT LQL_QEMU_CLQL)
  message(FATAL_ERROR "QEMU smoke requires an extracted SDK or CLI")
endif()

if(LQL_QEMU_SDK_PREFIX)
  enable_language(C)
  find_package(liblql CONFIG REQUIRED
    PATHS "${LQL_QEMU_SDK_PREFIX}/lib/cmake/liblql"
    NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  if(NOT liblql_VERSION STREQUAL LQL_QEMU_VERSION)
    message(FATAL_ERROR "Extracted SDK version mismatch: ${liblql_VERSION}")
  endif()
  find_package(Threads REQUIRED)
  include("${root}/cmake/LqlDevelopmentRuntime.cmake")
  # CTest applies the target's emulator; host build tools never inherit target
  # library paths. RPATH and the interpreter are private to these consumers.
  set(CMAKE_CROSSCOMPILING_EMULATOR
    "${CMAKE_COMMAND};-E;env;--unset=LD_LIBRARY_PATH;--unset=LD_PRELOAD;${LQL_QEMU_RUNNER};-L;${CPKT_SYSROOT}")
  foreach(kind shared static)
    add_executable(qemu_sdk_${kind} consumer.c)
    target_compile_options(qemu_sdk_${kind} PRIVATE
      -std=c89 -Wall -Wextra -Wpedantic -pedantic-errors -Werror)
    target_link_libraries(qemu_sdk_${kind} PRIVATE liblql::lql_${kind} Threads::Threads)
    lql_configure_development_runtime(qemu_sdk_${kind} "${LQL_QEMU_SDK_PREFIX}/lib")
    add_test(NAME qemu.${LQL_TARGET_ID}.sdk.${kind} COMMAND qemu_sdk_${kind})
  endforeach()
endif()

if(LQL_QEMU_CLQL)
  if(NOT EXISTS "${LQL_QEMU_CLQL}")
    message(FATAL_ERROR "Extracted CLI missing: ${LQL_QEMU_CLQL}")
  endif()
  add_test(NAME qemu.${LQL_TARGET_ID}.cli
    COMMAND "${CMAKE_COMMAND}"
      "-DRUNNER=${LQL_QEMU_RUNNER}" "-DSYSROOT=${CPKT_SYSROOT}"
      "-DCLQL=${LQL_QEMU_CLQL}" "-DEXPECTED_VERSION=${LQL_QEMU_VERSION}"
      "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cli.cmake")
endif()
get_property(tests DIRECTORY PROPERTY TESTS)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tmp")
set_tests_properties(${tests} PROPERTIES
  TIMEOUT 30 RUN_SERIAL TRUE LABELS "smoke;packaging;local"
  ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/tmp")
include("${root}/cmake/LqlTestContract.cmake")
lql_verify_test_contract()
