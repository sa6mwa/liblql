cmake_minimum_required(VERSION 3.20)

project(liblql VERSION 0.0.0 LANGUAGES C)

include(CTest)

if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR
    "liblql builds require GCC; Clang is not a supported build or performance compiler")
endif()

function(lql_configure_c89_target target)
  target_compile_options(${target} PRIVATE
    -std=c89
    -Wall
    -Wextra
    -Wpedantic
    -pedantic-errors
    -Werror)
endfunction()

function(lql_allow_test_partial_initializers target)
  target_compile_options(${target} PRIVATE
    -Wno-missing-field-initializers)
endfunction()

option(LQL_BUILD_STATIC "Build static liblql" ON)
option(LQL_BUILD_SHARED "Build shared liblql" ON)
option(LQL_BUILD_DIRECT_PROBE "Build the direct-execution architecture probe" ON)
set(LQL_VERSION_OVERRIDE "" CACHE STRING "Version override")

if(NOT LQL_BUILD_STATIC AND NOT LQL_BUILD_SHARED)
  message(FATAL_ERROR "Enable LQL_BUILD_STATIC or LQL_BUILD_SHARED")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(LqlVersion)

configure_file(cmake/lql_version.h.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/include/lql/version.h" @ONLY)

add_library(lql_common OBJECT
  src/lql.c
  src/lql_allocator.c
  src/lql_direct_actions.c
  src/lql_json_scan.c
  src/lql_json_spool.c
  src/lql_projection.c
  src/lql_selector.c
  src/lql_stream.c
  src/lql_stream_flat_eq.c
  src/lql_temporal.c
  src/lql_unicode_lower.c)
set_target_properties(lql_common PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(lql_common PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>")
lql_configure_c89_target(lql_common)

if(LQL_BUILD_STATIC)
  add_library(lql_static STATIC $<TARGET_OBJECTS:lql_common>)
  add_library(lql::static ALIAS lql_static)
  set_target_properties(lql_static PROPERTIES OUTPUT_NAME lql)
  target_include_directories(lql_static PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>")
endif()

if(LQL_BUILD_SHARED)
  add_library(lql_shared SHARED $<TARGET_OBJECTS:lql_common>)
  add_library(lql::shared ALIAS lql_shared)
  set_target_properties(lql_shared PROPERTIES OUTPUT_NAME lql SOVERSION 0)
  target_include_directories(lql_shared PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>")
endif()

if(BUILD_TESTING)
  add_executable(lql_json_scan_test tests/json_scan_test.c)
  target_include_directories(lql_json_scan_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(lql_json_scan_test PRIVATE lql_static)
  lql_configure_c89_target(lql_json_scan_test)
  lql_allow_test_partial_initializers(lql_json_scan_test)
  add_test(NAME lql.json_scan COMMAND lql_json_scan_test)
  add_executable(lql_direct_stream_test tests/direct_stream_test.c)
  target_link_libraries(lql_direct_stream_test PRIVATE lql_static)
  lql_configure_c89_target(lql_direct_stream_test)
  add_test(NAME lql.direct_stream COMMAND lql_direct_stream_test)
endif()

if(LQL_BUILD_DIRECT_PROBE)
  add_executable(lql_direct_probe tests/lql_direct_probe.c)
  target_compile_definitions(lql_direct_probe PRIVATE _POSIX_C_SOURCE=200809L)
  target_link_libraries(lql_direct_probe PRIVATE lql_static)
  lql_configure_c89_target(lql_direct_probe)
  add_executable(lql_direct_bench tools/lql_direct_bench.c)
  target_compile_definitions(lql_direct_bench PRIVATE _POSIX_C_SOURCE=200809L)
  target_link_libraries(lql_direct_bench PRIVATE lql_static)
  lql_configure_c89_target(lql_direct_bench)
endif()
