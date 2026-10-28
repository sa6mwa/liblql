cmake_minimum_required(VERSION 3.20)
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(work "${root}/build/qemu-contract")
execute_process(COMMAND sh "${root}/scripts/remove_path.sh" "${work}"
  COMMAND_ERROR_IS_FATAL ANY)
file(MAKE_DIRECTORY "${work}/sysroot")
file(WRITE "${work}/clql" "fixture")
file(WRITE "${work}/runner" "#!/bin/sh\necho 'intentional QEMU failure' >&2\nexit 42\n")
file(CHMOD "${work}/runner" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)

function(configure name)
  execute_process(COMMAND "${CMAKE_COMMAND}"
    -S "${root}/tests/qemu" -B "${work}/${name}" -G Ninja
    -DLQL_TARGET_ID=aarch64-linux-gnu -DCPKT_SOURCE=bootlin
    "-DCPKT_SYSROOT=${work}/sysroot" "-DLQL_QEMU_CLQL=${work}/clql"
    "-DLQL_QEMU_RUNNER=${work}/runner" -DLQL_QEMU_VERSION=0.0.0 ${ARGN}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  set(result "${result}" PARENT_SCOPE)
  set(log "${output}\n${error}" PARENT_SCOPE)
endfunction()

configure(missing-runner "-DLQL_QEMU_RUNNER=${work}/absent")
if(result EQUAL 0 OR NOT log MATCHES "QEMU runner missing")
  message(FATAL_ERROR "Missing QEMU runner was not rejected: ${log}")
endif()
configure(missing-sysroot "-DCPKT_SYSROOT=${work}/absent")
if(result EQUAL 0 OR NOT log MATCHES "selected Bootlin sysroot")
  message(FATAL_ERROR "Missing QEMU sysroot was not rejected: ${log}")
endif()
configure(failing-runner)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Runner failure fixture did not configure: ${log}")
endif()
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${work}/failing-runner"
  --stop-on-failure --output-on-failure --no-tests=error
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR NOT output MATCHES "status=42" OR
   NOT output MATCHES "intentional QEMU failure")
  message(FATAL_ERROR "Runner failure did not fail CTest: ${output}\n${error}")
endif()
message(STATUS "QEMU contract: missing runner, missing sysroot and runner failure rejected")
