cmake_minimum_required(VERSION 3.20)

project(liblql VERSION 0.0.0 LANGUAGES C)

include(CTest)
include(CheckIPOSupported)

option(LQL_BUILD_STATIC "Build static liblql" ON)
option(LQL_BUILD_SHARED "Build shared liblql" ON)
option(LQL_BUILD_DIRECT_PROBE "Build the direct-execution architecture probe" ON)
set(LQL_VERSION_OVERRIDE "" CACHE STRING "Version override")

if(NOT LQL_BUILD_STATIC AND NOT LQL_BUILD_SHARED)
  message(FATAL_ERROR "Enable LQL_BUILD_STATIC or LQL_BUILD_SHARED")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(LqlVersion)
include(LqlLonejson)

configure_file(cmake/lql_version.h.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/include/lql/version.h" @ONLY)

set(CMAKE_C_STANDARD 90)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(lql_common OBJECT
  src/lql.c
  src/lql_allocator.c
  src/lql_selector.c
  src/lql_stream.c
  src/lql_temporal.c)
set_target_properties(lql_common PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(lql_common PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>")
target_link_libraries(lql_common PUBLIC lonejson::lonejson)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(lql_common PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(LQL_BUILD_STATIC)
  add_library(lql_static STATIC $<TARGET_OBJECTS:lql_common>)
  add_library(lql::static ALIAS lql_static)
  set_target_properties(lql_static PROPERTIES OUTPUT_NAME lql)
  target_link_libraries(lql_static PUBLIC lonejson::lonejson)
  target_include_directories(lql_static PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>")
endif()

if(LQL_BUILD_SHARED)
  add_library(lql_shared SHARED $<TARGET_OBJECTS:lql_common>)
  add_library(lql::shared ALIAS lql_shared)
  set_target_properties(lql_shared PROPERTIES OUTPUT_NAME lql SOVERSION 0)
  target_link_libraries(lql_shared PUBLIC lonejson::lonejson)
  target_include_directories(lql_shared PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>")
endif()

if(BUILD_TESTING)
  add_executable(lql_direct_stream_test tests/direct_stream_test.c)
  target_link_libraries(lql_direct_stream_test PRIVATE lql_static)
  add_test(NAME lql.direct_stream COMMAND lql_direct_stream_test)
endif()

if(LQL_BUILD_DIRECT_PROBE)
  add_executable(lql_direct_probe tests/lql_direct_probe.c)
  target_compile_definitions(lql_direct_probe PRIVATE _POSIX_C_SOURCE=200809L)
  target_link_libraries(lql_direct_probe PRIVATE lql_static)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  check_ipo_supported(RESULT lql_ipo_supported OUTPUT lql_ipo_error LANGUAGES C)
  if(lql_ipo_supported)
    set_property(TARGET lql_common lonejson_vendored
      PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    if(TARGET lql_static)
      set_property(TARGET lql_static PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
    if(TARGET lql_shared)
      set_property(TARGET lql_shared PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
    if(TARGET lql_direct_probe)
      set_property(TARGET lql_direct_probe PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
  else()
    message(STATUS "liblql IPO unavailable: ${lql_ipo_error}")
  endif()
endif()
