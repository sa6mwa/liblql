file(MAKE_DIRECTORY "${WORK}")
set(input "${WORK}/input.ndjson")
file(WRITE "${input}" "{\"kind\":\"commit\",\"n\":1.5}\n{\"kind\":\"other\",\"n\":2}\n")
file(WRITE "${WORK}/invalid.ndjson" "{\"kind\":}\n")

function(check label expected_status expected_output)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH --unset=LD_PRELOAD
      "${RUNNER}" -L "${SYSROOT}" "${CLQL}" ${ARGN}
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
  if(NOT "${status}" STREQUAL "${expected_status}" OR
     NOT "${output}" STREQUAL "${expected_output}")
    message(FATAL_ERROR "QEMU CLI ${label}: status=${status}, expected=${expected_status}\noutput=${output}\n${error}")
  endif()
endfunction()

check(version 0 "${EXPECTED_VERSION}\n" --version)
check(selection 0 "{\"kind\":\"commit\",\"n\":1.5}\n" "/kind=\"commit\"" "${input}")
check(projection 0 "{\"n\":1.5}\n" -f /n "/kind=\"commit\"" "${input}")
check(mutation 0 "{\"kind\":\"ready\",\"n\":1.5}\n{\"kind\":\"other\",\"n\":2}\n"
  -m /kind=ready "/kind=\"commit\"" "${input}")
check(invalid-json 1 "" "/kind=\"commit\"" "${WORK}/invalid.ndjson")
