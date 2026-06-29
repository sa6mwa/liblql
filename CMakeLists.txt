cmake_minimum_required(VERSION 3.20)

project(liblql VERSION 0.0.0 LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(LQL_BUILD_STATIC "Build static liblql" ON)
option(LQL_BUILD_SHARED "Build shared liblql" ON)
option(LQL_BUILD_BINARY "Build clql" ON)
option(LQL_CLQL_STATIC_LINK "Link clql as a static executable where supported" OFF)
option(LQL_BUILD_TESTS "Build tests" ON)
option(LQL_BUILD_EXAMPLES "Build examples" ON)
option(LQL_BUILD_BENCHMARKS "Build benchmark helpers" ON)
option(LQL_BUILD_LUA_MODULE "Build direct Lua C module when Lua headers exist" ON)
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

configure_file(cmake/lql_version.h.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/include/lql/version.h"
  @ONLY)

set(CMAKE_C_STANDARD 90)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(LQL_PROJECT_WARNINGS)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  set(LQL_PROJECT_WARNINGS -Wall -Wextra -Wpedantic -Werror)
endif()

if(LQL_TARGET_OS STREQUAL "darwin")
  set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
endif()

function(lql_apply_project_warnings target)
  if(LQL_PROJECT_WARNINGS)
    target_compile_options(${target} PRIVATE ${LQL_PROJECT_WARNINGS})
  endif()
endfunction()

add_library(lql_common OBJECT
  src/lql.c
  src/lql_allocator.c
  src/lql_selector.c
  src/lql_eval.c
  src/lql_project.c
  src/lql_mutation.c
  src/lql_temporal.c)
set_target_properties(lql_common PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(lql_common
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>"
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
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
endif()

if(LQL_BUILD_SHARED)
  add_library(lql_shared SHARED $<TARGET_OBJECTS:lql_common>)
  add_library(lql::shared ALIAS lql_shared)
  set_target_properties(lql_shared PROPERTIES
    OUTPUT_NAME lql
    VERSION "${PROJECT_VERSION}"
    SOVERSION 0)
  if(LQL_TARGET_OS STREQUAL "darwin")
    set_target_properties(lql_shared PROPERTIES
      MACOSX_RPATH ON
      INSTALL_NAME_DIR "@rpath"
      INSTALL_RPATH "@loader_path")
  endif()
  target_link_libraries(lql_shared PUBLIC lonejson::lonejson)
  target_include_directories(lql_shared PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
endif()

if(LQL_BUILD_BINARY)
  if(LQL_CLQL_STATIC_LINK)
    if(NOT TARGET lonejson::lonejson_static)
      message(FATAL_ERROR "LQL_CLQL_STATIC_LINK requires lonejson::lonejson_static")
    endif()
    add_executable(clql src/clql.c $<TARGET_OBJECTS:lql_common>)
    target_link_libraries(clql PRIVATE lonejson::lonejson_static)
    target_include_directories(clql PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/include"
      "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
  else()
    add_executable(clql src/clql.c)
  endif()
  if(NOT LQL_CLQL_STATIC_LINK AND LQL_BUILD_STATIC)
    target_link_libraries(clql PRIVATE lql_static)
  elseif(NOT LQL_CLQL_STATIC_LINK)
    target_link_libraries(clql PRIVATE lql_common)
  endif()
  lql_apply_project_warnings(clql)
  if(LQL_CLQL_STATIC_LINK AND LQL_TARGET_OS STREQUAL "linux")
    target_link_options(clql PRIVATE -static)
  endif()
  if(NOT LQL_CLQL_STATIC_LINK)
    if(LQL_TARGET_OS STREQUAL "darwin")
      set_target_properties(clql PROPERTIES INSTALL_RPATH "@executable_path/../lib")
    else()
      set_target_properties(clql PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
    endif()
  endif()

  if(LQL_BUILD_BENCHMARKS)
    add_executable(lql_payload_bench bench/lql_payload_bench.c)
    target_link_libraries(lql_payload_bench PRIVATE $<IF:$<BOOL:${LQL_BUILD_STATIC}>,lql_static,lql_shared>)
    lql_apply_project_warnings(lql_payload_bench)
  endif()
endif()

if(LQL_BUILD_EXAMPLES)
  add_executable(lql_match_example examples/match.c)
  target_link_libraries(lql_match_example PRIVATE $<IF:$<BOOL:${LQL_BUILD_STATIC}>,lql_static,lql_shared>)
  lql_apply_project_warnings(lql_match_example)
endif()

if(LQL_BUILD_LUA_MODULE)
  find_path(LQL_LUA_INCLUDE_DIR lua.h
    PATH_SUFFIXES lua5.5 lua)
  if(LQL_LUA_INCLUDE_DIR)
    file(STRINGS "${LQL_LUA_INCLUDE_DIR}/lua.h" LQL_LUA_VERSION_55_NUM
      REGEX "^[ \t]*#define[ \t]+LUA_VERSION_NUM[ \t]+505[ \t]*$")
    file(STRINGS "${LQL_LUA_INCLUDE_DIR}/lua.h" LQL_LUA_VERSION_55_MAJOR
      REGEX "^[ \t]*#define[ \t]+LUA_VERSION_MAJOR_N[ \t]+5[ \t]*$")
    file(STRINGS "${LQL_LUA_INCLUDE_DIR}/lua.h" LQL_LUA_VERSION_55_MINOR
      REGEX "^[ \t]*#define[ \t]+LUA_VERSION_MINOR_N[ \t]+5[ \t]*$")
    if(NOT LQL_LUA_VERSION_55_NUM AND
       NOT (LQL_LUA_VERSION_55_MAJOR AND LQL_LUA_VERSION_55_MINOR))
      message(STATUS "Lua headers at ${LQL_LUA_INCLUDE_DIR} are not Lua 5.5; direct Lua C module disabled")
      set(LQL_LUA_INCLUDE_DIR "")
    endif()
  endif()
  if(LQL_LUA_INCLUDE_DIR)
    if(TARGET lql_shared)
      set(LQL_LUA_LINK_TARGET lql_shared)
    else()
      set(LQL_LUA_LINK_TARGET "")
    endif()
    if(LQL_LUA_LINK_TARGET)
      add_library(lql_lua_core MODULE lua/lql_core.c)
      set_target_properties(lql_lua_core PROPERTIES
        PREFIX ""
        OUTPUT_NAME core
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lql"
        C_EXTENSIONS ON)
      target_include_directories(lql_lua_core PRIVATE "${LQL_LUA_INCLUDE_DIR}")
      target_link_libraries(lql_lua_core PRIVATE ${LQL_LUA_LINK_TARGET})
      lql_apply_project_warnings(lql_lua_core)
    else()
      message(STATUS "Shared liblql target not available; direct Lua C module disabled")
    endif()
  else()
    message(STATUS "Lua headers not found; direct Lua C module disabled")
  endif()
endif()

include(CTest)
if(LQL_BUILD_TESTS)
  enable_testing()
  add_executable(test_lql tests/test_lql.c)
  target_link_libraries(test_lql PRIVATE $<IF:$<BOOL:${LQL_BUILD_STATIC}>,lql_static,lql_shared>)
  lql_apply_project_warnings(test_lql)
  add_test(NAME lql.unit COMMAND test_lql)
  set_tests_properties(lql.unit PROPERTIES LABELS "unit;local" TIMEOUT 2)

  add_executable(lql_handle_allocator_test tests/selector_allocator_test.c)
  target_include_directories(lql_handle_allocator_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(lql_handle_allocator_test PRIVATE lql_common)
  lql_apply_project_warnings(lql_handle_allocator_test)
  add_test(NAME lql.handle-allocator COMMAND lql_handle_allocator_test)
  set_tests_properties(lql.handle-allocator PROPERTIES LABELS "unit;api"
    TIMEOUT 5)

  add_executable(lql_fuzz_smoke tests/fuzz_smoke.c)
  target_link_libraries(lql_fuzz_smoke PRIVATE $<IF:$<BOOL:${LQL_BUILD_STATIC}>,lql_static,lql_shared>)
  lql_apply_project_warnings(lql_fuzz_smoke)
  add_test(NAME lql.fuzz-smoke COMMAND lql_fuzz_smoke)
  set_tests_properties(lql.fuzz-smoke PROPERTIES LABELS "fuzz;local"
    TIMEOUT 5)

  add_test(NAME lql.sdk-manifest
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_sdk_unit_manifest.sh
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lql.c
      ${CMAKE_CURRENT_SOURCE_DIR}/include/lql/lql.h)
  set_tests_properties(lql.sdk-manifest PROPERTIES LABELS "unit;api" TIMEOUT 5)
  add_test(NAME lql.sdk-manifest-fixtures
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_sdk_unit_manifest.sh
      --fixtures)
  set_tests_properties(lql.sdk-manifest-fixtures PROPERTIES
    LABELS "unit;api" TIMEOUT 5)

  if(TARGET lql_shared)
    set(LQL_PUBLIC_API_STYLE_SHARED $<TARGET_FILE:lql_shared>)
  else()
    set(LQL_PUBLIC_API_STYLE_SHARED "")
  endif()
  add_test(NAME lql.public-api-style
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_public_api_style.sh
      ${CMAKE_CURRENT_SOURCE_DIR}/include/lql/lql.h
      ${LQL_PUBLIC_API_STYLE_SHARED}
      ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(lql.public-api-style PROPERTIES LABELS "unit;api" TIMEOUT 5)
  add_test(NAME lql.public-api-style-fixtures
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_public_api_style_fixtures.sh
      ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_public_api_style.sh)
  set_tests_properties(lql.public-api-style-fixtures PROPERTIES
    LABELS "unit;api" TIMEOUT 15)

  add_test(NAME lql.release-surface
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_make_release_target.sh
      ${CMAKE_CURRENT_SOURCE_DIR}/Makefile
      ${CMAKE_CURRENT_SOURCE_DIR}/scripts/release_gate.sh
      ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt)
  set_tests_properties(lql.release-surface PROPERTIES
    LABELS "unit;packaging" TIMEOUT 5)
  add_test(NAME lql.release-surface-fixtures
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_make_release_target.sh
      --fixtures)
  set_tests_properties(lql.release-surface-fixtures PROPERTIES
    LABELS "unit;packaging" TIMEOUT 5)
  add_test(NAME lql.release-version
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_release_version.sh
      ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(lql.release-version PROPERTIES
    LABELS "unit;packaging" TIMEOUT 20)

  add_test(NAME lql.repository-boundaries
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_repository_boundaries.sh
      ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(lql.repository-boundaries PROPERTIES
    LABELS "unit;api" TIMEOUT 5)
  add_test(NAME lql.repository-boundary-fixtures
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_repository_boundaries.sh
      --fixtures)
  set_tests_properties(lql.repository-boundary-fixtures PROPERTIES
    LABELS "unit;api" TIMEOUT 5)

  add_test(NAME lql.clean-fixtures
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/clean.sh --fixtures)
  set_tests_properties(lql.clean-fixtures PROPERTIES
    LABELS "unit;operability" TIMEOUT 5)

  add_test(NAME lql.script-modes
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_script_modes.sh
      ${CMAKE_CURRENT_SOURCE_DIR})
  set_tests_properties(lql.script-modes PROPERTIES
    LABELS "unit;operability" TIMEOUT 5)
  add_test(NAME lql.script-modes-fixtures
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_script_modes.sh
      --fixtures)
  set_tests_properties(lql.script-modes-fixtures PROPERTIES
    LABELS "unit;operability" TIMEOUT 5)
  add_test(NAME lql.install-surface
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_make_install_target.sh
      ${CMAKE_CURRENT_SOURCE_DIR}/Makefile
      ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_clql_static.sh
      ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt)
  set_tests_properties(lql.install-surface PROPERTIES
    LABELS "unit;operability" TIMEOUT 5)
  add_test(NAME lql.install-surface-fixtures
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_make_install_target.sh
      --fixtures)
  set_tests_properties(lql.install-surface-fixtures PROPERTIES
    LABELS "unit;operability" TIMEOUT 5)

  add_test(NAME lql.cmake-presets
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_cmake_presets.sh
      ${CMAKE_CURRENT_SOURCE_DIR}/CMakePresets.json)
  set_tests_properties(lql.cmake-presets PROPERTIES
    LABELS "unit;operability" TIMEOUT 5)
  add_test(NAME lql.cmake-presets-fixtures
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_cmake_presets.sh
      --fixtures)
  set_tests_properties(lql.cmake-presets-fixtures PROPERTIES
    LABELS "unit;operability" TIMEOUT 5)

  add_test(NAME lql.target-tools-fixtures
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/discover_target_tools.sh
      --fixtures)
  set_tests_properties(lql.target-tools-fixtures PROPERTIES
    LABELS "unit;operability;packaging" TIMEOUT 5)

  add_test(NAME lql.package-privacy-fixtures
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package.sh
      package-privacy-fixtures)
  set_tests_properties(lql.package-privacy-fixtures PROPERTIES
    LABELS "unit;packaging" TIMEOUT 5)

  add_test(NAME lql.package-manifest-fixtures
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package.sh
      package-manifest-fixtures)
  set_tests_properties(lql.package-manifest-fixtures PROPERTIES
    LABELS "unit;packaging" TIMEOUT 5)
  add_test(NAME lql.package-tool-path-fixtures
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package.sh
      package-tool-path-fixtures)
  set_tests_properties(lql.package-tool-path-fixtures PROPERTIES
    LABELS "unit;operability;packaging" TIMEOUT 5)
  add_test(NAME lql.package-version-fixtures
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package.sh
      package-version-fixtures)
  set_tests_properties(lql.package-version-fixtures PROPERTIES
    LABELS "unit;packaging" TIMEOUT 5)
  add_test(NAME lql.package-lua-contract-fixtures
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package.sh
      package-lua-contract-fixtures)
  set_tests_properties(lql.package-lua-contract-fixtures PROPERTIES
    LABELS "unit;packaging;lua" TIMEOUT 5)
  add_test(NAME lql.package-source-manifest-fixtures
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package.sh
      package-source-manifest-fixtures)
  set_tests_properties(lql.package-source-manifest-fixtures PROPERTIES
    LABELS "unit;packaging" TIMEOUT 5)

  set(LQL_HEADER_SMOKE_WARNINGS)
  if(LQL_PROJECT_WARNINGS)
    set(LQL_HEADER_SMOKE_WARNINGS ${LQL_PROJECT_WARNINGS})
  endif()
  add_test(NAME lql.header COMMAND
    ${CMAKE_C_COMPILER} -std=c90 ${LQL_HEADER_SMOKE_WARNINGS}
    -I${CMAKE_CURRENT_SOURCE_DIR}/include
    -I${CMAKE_CURRENT_BINARY_DIR}/generated/include
    -c ${CMAKE_CURRENT_SOURCE_DIR}/tests/header_smoke.c -o ${CMAKE_CURRENT_BINARY_DIR}/header_smoke.o)
  set_tests_properties(lql.header PROPERTIES LABELS "unit;api" TIMEOUT 5)

  add_test(NAME lql.header-cxx
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_cpp_header_smoke.sh
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/header_smoke.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_BINARY_DIR}/generated/include
      ${CMAKE_CURRENT_BINARY_DIR}/header_smoke_cpp.o)
  set_tests_properties(lql.header-cxx PROPERTIES LABELS "unit;api" TIMEOUT 5)

  find_program(GO_EXECUTABLE go)
  if(GO_EXECUTABLE AND LQL_BUILD_BINARY)
    add_test(NAME lql.parity
      COMMAND ${CMAKE_COMMAND} -E env
        CLQL_PATH=$<TARGET_FILE:clql>
        ${GO_EXECUTABLE} test ./...
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/parity)
    set_tests_properties(lql.parity PROPERTIES LABELS "parity;local" TIMEOUT 60)
  endif()
  set(LQL_SANITIZER_BUILD OFF)
  if(CMAKE_C_FLAGS MATCHES "-fsanitize")
    set(LQL_SANITIZER_BUILD ON)
  endif()
  if(GO_EXECUTABLE AND LQL_BUILD_SHARED AND NOT LQL_SANITIZER_BUILD)
    add_test(NAME lql.sdk-parity
      COMMAND ${CMAKE_COMMAND} -E env
        CGO_ENABLED=1
        "CGO_CFLAGS=-I${CMAKE_CURRENT_SOURCE_DIR}/include -I${CMAKE_CURRENT_BINARY_DIR}/generated/include"
        "CGO_LDFLAGS=-L$<TARGET_FILE_DIR:lql_shared> -L${LQL_EXTERNAL_ROOT}/lib -llql -llonejson"
        "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:lql_shared>:${LQL_EXTERNAL_ROOT}/lib"
        "DYLD_LIBRARY_PATH=$<TARGET_FILE_DIR:lql_shared>:${LQL_EXTERNAL_ROOT}/lib"
        ${GO_EXECUTABLE} test -tags liblql_sdk_parity ./...
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/parity)
    set_tests_properties(lql.sdk-parity PROPERTIES LABELS "parity;api;local" TIMEOUT 60)
  endif()
  find_program(LUA_EXECUTABLE NAMES lua5.5 lua)
  set(LQL_LUA_EXECUTABLE_IS_55 OFF)
  if(LUA_EXECUTABLE)
    execute_process(COMMAND ${LUA_EXECUTABLE} -e "print(_VERSION)"
      OUTPUT_VARIABLE LQL_LUA_VERSION_OUTPUT
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(LQL_LUA_VERSION_OUTPUT STREQUAL "Lua 5.5")
      set(LQL_LUA_EXECUTABLE_IS_55 ON)
    else()
      message(STATUS "Lua executable ${LUA_EXECUTABLE} is not Lua 5.5; Lua CTest smoke disabled")
    endif()
  endif()
  if(LUA_EXECUTABLE AND LQL_LUA_EXECUTABLE_IS_55 AND TARGET lql_lua_core AND NOT LQL_SANITIZER_BUILD)
    add_test(NAME lql.lua-core
      COMMAND ${CMAKE_COMMAND} -E env
        "LUA_PATH=${CMAKE_CURRENT_SOURCE_DIR}/lua/?.lua$<SEMICOLON>${CMAKE_CURRENT_SOURCE_DIR}/lua/?/init.lua$<SEMICOLON>$<SEMICOLON>"
        "LUA_CPATH=${CMAKE_CURRENT_BINARY_DIR}/?.so$<SEMICOLON>${CMAKE_CURRENT_BINARY_DIR}/?/core.so$<SEMICOLON>$<SEMICOLON>"
        "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:lql_shared>:${LQL_EXTERNAL_ROOT}/lib"
        "DYLD_LIBRARY_PATH=$<TARGET_FILE_DIR:lql_shared>:${LQL_EXTERNAL_ROOT}/lib"
        LQL_REQUIRE_CORE=1
        ${LUA_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/lua/tests/lql_core_smoke.lua)
    set_tests_properties(lql.lua-core PROPERTIES LABELS "unit;api;lua" TIMEOUT 5)
  endif()
  add_test(NAME lql.lua-runtime-fixtures
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_lua_runtime_fixtures.sh)
  set_tests_properties(lql.lua-runtime-fixtures PROPERTIES
    LABELS "unit;api;lua" TIMEOUT 5)
  if(GO_EXECUTABLE AND LUA_EXECUTABLE AND LQL_LUA_EXECUTABLE_IS_55 AND
      TARGET clql AND TARGET lql_payload_bench AND TARGET lql_lua_core AND
      TARGET lql_shared AND NOT LQL_SANITIZER_BUILD)
    add_test(NAME lql.benchmark-1g-fixtures
      COMMAND ${CMAKE_COMMAND} -E env
        GO=${GO_EXECUTABLE}
        LUA=${LUA_EXECUTABLE}
        CLQL_PATH=$<TARGET_FILE:clql>
        LQL_PAYLOAD_BENCH_PATH=$<TARGET_FILE:lql_payload_bench>
        LQL_LUA_MODULE_DIR=${CMAKE_CURRENT_BINARY_DIR}
        LQL_BENCH_LIBRARY_DIR=$<TARGET_FILE_DIR:lql_shared>
        LQL_BENCH_DEP_LIBRARY_DIR=${LQL_EXTERNAL_ROOT}/lib
        sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_parity_benchmark_1g_fixtures.sh)
    set_tests_properties(lql.benchmark-1g-fixtures PROPERTIES
      LABELS "unit;local;lua" TIMEOUT 20)
  endif()
  if(TARGET lql_match_example)
    add_test(NAME lql.example-match COMMAND lql_match_example)
    set_tests_properties(lql.example-match PROPERTIES
      LABELS "example-smoke;api" TIMEOUT 5)
  endif()
  if(TARGET clql)
    add_test(NAME lql.cli-smoke
      COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_clql_smoke.sh
        $<TARGET_FILE:clql> ${LQL_RESOLVED_VERSION})
    set_tests_properties(lql.cli-smoke PROPERTIES
      LABELS "smoke;api" TIMEOUT 5)
  endif()
endif()

if(LQL_INSTALL)
  install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/include/lql/version.h"
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/lql")
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
