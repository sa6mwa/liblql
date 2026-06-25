cmake_minimum_required(VERSION 3.20)

project(liblql VERSION 0.0.0 LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(LQL_BUILD_STATIC "Build static liblql" ON)
option(LQL_BUILD_SHARED "Build shared liblql" ON)
option(LQL_BUILD_BINARY "Build clql" ON)
option(LQL_BUILD_TESTS "Build tests" ON)
option(LQL_BUILD_EXAMPLES "Build examples" ON)
option(LQL_INSTALL "Install liblql artifacts" ON)
set(LQL_EXTERNAL_ROOT "" CACHE PATH "Dependency SDK root")
set(LQL_TARGET_ID "" CACHE STRING "Release target id")
set(LQL_TARGET_ARCH "" CACHE STRING "Release target architecture")
set(LQL_TARGET_OS "" CACHE STRING "Release target OS")
set(LQL_TARGET_LIBC "" CACHE STRING "Release target libc")
set(LQL_DIST_DIR "${CMAKE_SOURCE_DIR}/dist" CACHE PATH "Distribution output directory")
set(LQL_DEPENDENCY_MODE "bundled" CACHE STRING "Dependency mode: bundled, host, auto")
set(LQL_VERSION_OVERRIDE "" CACHE STRING "Version override for release candidates")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(LqlVersion)
include(LqlLonejson)

set(CMAKE_C_STANDARD 90)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(LQL_PROJECT_WARNINGS)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  set(LQL_PROJECT_WARNINGS -Wall -Wextra -Wpedantic -Werror)
endif()

function(lql_apply_project_warnings target)
  if(LQL_PROJECT_WARNINGS)
    target_compile_options(${target} PRIVATE ${LQL_PROJECT_WARNINGS})
  endif()
endfunction()

add_library(lql_common OBJECT
  src/lql.c
  src/lql_selector.c
  src/lql_eval.c
  src/lql_project.c
  src/lql_mutation.c
  src/lql_temporal.c)
set_target_properties(lql_common PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(lql_common
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(lql_common PUBLIC lonejson::lonejson)
lql_apply_project_warnings(lql_common)

if(LQL_BUILD_STATIC)
  add_library(lql_static STATIC $<TARGET_OBJECTS:lql_common>)
  add_library(lql::static ALIAS lql_static)
  set_target_properties(lql_static PROPERTIES OUTPUT_NAME lql)
  target_link_libraries(lql_static PUBLIC lonejson::lonejson)
  target_include_directories(lql_static PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
endif()

if(LQL_BUILD_SHARED)
  add_library(lql_shared SHARED $<TARGET_OBJECTS:lql_common>)
  add_library(lql::shared ALIAS lql_shared)
  set_target_properties(lql_shared PROPERTIES
    OUTPUT_NAME lql
    VERSION "${PROJECT_VERSION}"
    SOVERSION 0)
  target_link_libraries(lql_shared PUBLIC lonejson::lonejson)
  target_include_directories(lql_shared PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
endif()

if(LQL_BUILD_BINARY)
  add_executable(clql src/clql.c)
  target_link_libraries(clql PRIVATE $<IF:$<BOOL:${LQL_BUILD_STATIC}>,lql_static,lql_shared>)
  lql_apply_project_warnings(clql)
endif()

if(LQL_BUILD_EXAMPLES)
  add_executable(lql_match_example examples/match.c)
  target_link_libraries(lql_match_example PRIVATE $<IF:$<BOOL:${LQL_BUILD_STATIC}>,lql_static,lql_shared>)
  lql_apply_project_warnings(lql_match_example)
endif()

include(CTest)
if(LQL_BUILD_TESTS)
  enable_testing()
  add_executable(test_lql tests/test_lql.c)
  target_link_libraries(test_lql PRIVATE $<IF:$<BOOL:${LQL_BUILD_STATIC}>,lql_static,lql_shared>)
  lql_apply_project_warnings(test_lql)
  add_test(NAME lql.unit COMMAND test_lql)
  set_tests_properties(lql.unit PROPERTIES LABELS "unit;local" TIMEOUT 20)

  set(LQL_HEADER_SMOKE_WARNINGS)
  if(LQL_PROJECT_WARNINGS)
    set(LQL_HEADER_SMOKE_WARNINGS ${LQL_PROJECT_WARNINGS})
  endif()
  add_test(NAME lql.header COMMAND
    ${CMAKE_C_COMPILER} -std=c90 ${LQL_HEADER_SMOKE_WARNINGS}
    -I${CMAKE_CURRENT_SOURCE_DIR}/include
    -c ${CMAKE_CURRENT_SOURCE_DIR}/tests/header_smoke.c -o ${CMAKE_CURRENT_BINARY_DIR}/header_smoke.o)
  set_tests_properties(lql.header PROPERTIES LABELS "unit;api" TIMEOUT 20)

  find_program(GO_EXECUTABLE go)
  if(GO_EXECUTABLE AND LQL_BUILD_BINARY)
    add_test(NAME lql.parity
      COMMAND ${CMAKE_COMMAND} -E env
        CLQL_PATH=$<TARGET_FILE:clql>
        ${GO_EXECUTABLE} test ./...
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/parity)
    set_tests_properties(lql.parity PROPERTIES LABELS "parity;local" TIMEOUT 60)
  endif()
endif()

if(LQL_INSTALL)
  install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  if(LQL_BUILD_STATIC)
    install(TARGETS lql_static EXPORT liblqlTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  endif()
  if(LQL_BUILD_SHARED)
    install(TARGETS lql_shared EXPORT liblqlTargets LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  endif()
  if(LQL_BUILD_BINARY)
    install(TARGETS clql RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  endif()
  install(FILES LICENSE README.md DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/liblql")
  install(EXPORT liblqlTargets NAMESPACE liblql:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/liblql")
  configure_package_config_file(cmake/liblqlConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/liblqlConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/liblql")
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/liblqlConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/liblqlConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/liblqlConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/liblql")
  configure_file(cmake/liblql.pc.in "${CMAKE_CURRENT_BINARY_DIR}/liblql.pc" @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/liblql.pc" DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
endif()
