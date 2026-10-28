cmake_minimum_required(VERSION 3.20)
project(liblql_cli_parity NONE)
enable_testing()
get_filename_component(root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
find_program(GO_EXECUTABLE go REQUIRED)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cli_parity_test"
  COMMAND "${GO_EXECUTABLE}" test -c -o "${CMAKE_CURRENT_BINARY_DIR}/cli_parity_test"
          "${CMAKE_CURRENT_SOURCE_DIR}/parity_test.go"
  DEPENDS parity_test.go VERBATIM)
add_custom_target(cli_parity ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/cli_parity_test")
foreach(suite Selector Mutation LuaGo LuaC ClqlXattr)
  add_test(NAME cli.${suite} COMMAND "${CMAKE_CURRENT_BINARY_DIR}/cli_parity_test"
    -test.run "^Test${suite}$" -test.v -test.timeout=55s)
  set_tests_properties(cli.${suite} PROPERTIES LABELS "integration;local;parity"
    TIMEOUT 60 RUN_SERIAL TRUE ENVIRONMENT "LQL_TEST_ROOT=${root};TMPDIR=${root}/build/tmp")
endforeach()
add_test(NAME cli.EmptySelection COMMAND "${CMAKE_COMMAND}"
  -DBINARY=${CMAKE_CURRENT_BINARY_DIR}/cli_parity_test
  -P "${CMAKE_CURRENT_SOURCE_DIR}/empty.cmake")
set_tests_properties(cli.EmptySelection PROPERTIES TIMEOUT 5 LABELS "unit;local;parity")
