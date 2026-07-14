cmake_minimum_required(VERSION 3.20)

project(liblql VERSION 0.0.0 LANGUAGES C)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR
    "liblql Linux builds require lifecycle GCC; Clang is not a supported build or performance compiler")
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
option(LQL_BUILD_BINARY "Build clql command-line tool" ON)
option(LQL_CLQL_STATIC "Build clql as a fully static Linux executable" OFF)
option(LQL_BUILD_DIRECT_PROBE "Build the direct-execution architecture probe" ON)
option(LQL_BUILD_FUZZERS "Build AFL++ fuzz targets" OFF)
option(LQL_BUILD_LUA "Build Lua 5.5 facade module" OFF)
option(LQL_INSTALL "Install liblql SDK artifacts" ON)
set(LQL_LUA_SDK_PREFIX "" CACHE PATH "Installed liblql SDK prefix used to build the Lua module")
set(LQL_VERSION_OVERRIDE "" CACHE STRING "Version override")
set(LQL_DIST_DIR "dist" CACHE PATH "Release artifact output directory")

if(NOT LQL_BUILD_STATIC AND NOT LQL_BUILD_SHARED)
  message(FATAL_ERROR "Enable LQL_BUILD_STATIC or LQL_BUILD_SHARED")
endif()
if(LQL_CLQL_STATIC AND NOT LQL_BUILD_STATIC)
  message(FATAL_ERROR "LQL_CLQL_STATIC requires LQL_BUILD_STATIC")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(LqlDependencyCache)
include(LqlVersion)

configure_file(cmake/lql_version.h.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/include/lql/version.h" @ONLY)
configure_file(cmake/liblql.pc.in
  "${CMAKE_CURRENT_BINARY_DIR}/liblql.pc" @ONLY)
configure_package_config_file(cmake/liblqlConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/liblqlConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/liblql")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/liblqlConfigVersion.cmake"
  VERSION "${LQL_RESOLVED_VERSION}"
  COMPATIBILITY SameMajorVersion)

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
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
endif()

if(LQL_BUILD_SHARED)
  add_library(lql_shared SHARED $<TARGET_OBJECTS:lql_common>)
  add_library(lql::shared ALIAS lql_shared)
  set_target_properties(lql_shared PROPERTIES
    OUTPUT_NAME lql
    SOVERSION 0
    INSTALL_NAME_DIR "@rpath")
  target_include_directories(lql_shared PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
endif()

if(LQL_BUILD_STATIC)
  set(LQL_INTERNAL_LIBRARY lql_static)
else()
  set(LQL_INTERNAL_LIBRARY lql_shared)
endif()

if(BUILD_TESTING)
  add_executable(lql_json_scan_test tests/json_scan_test.c)
  target_include_directories(lql_json_scan_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  if(LQL_BUILD_STATIC)
    target_link_libraries(lql_json_scan_test PRIVATE lql_static)
  else()
    # This white-box scanner test deliberately exercises hidden internals.
    target_link_libraries(lql_json_scan_test PRIVATE lql_common)
  endif()
  lql_configure_c89_target(lql_json_scan_test)
  lql_allow_test_partial_initializers(lql_json_scan_test)
  add_test(NAME lql.json_scan COMMAND lql_json_scan_test)
  add_executable(lql_direct_stream_test tests/direct_stream_test.c)
  target_link_libraries(lql_direct_stream_test PRIVATE ${LQL_INTERNAL_LIBRARY})
  lql_configure_c89_target(lql_direct_stream_test)
  add_test(NAME lql.direct_stream COMMAND lql_direct_stream_test)
endif()

if(LQL_BUILD_DIRECT_PROBE)
  add_executable(lql_direct_probe tests/lql_direct_probe.c)
  target_compile_definitions(lql_direct_probe PRIVATE _POSIX_C_SOURCE=200809L)
  target_link_libraries(lql_direct_probe PRIVATE ${LQL_INTERNAL_LIBRARY})
  lql_configure_c89_target(lql_direct_probe)
  add_executable(lql_direct_bench tools/lql_direct_bench.c)
  target_compile_definitions(lql_direct_bench PRIVATE _POSIX_C_SOURCE=200809L)
  target_link_libraries(lql_direct_bench PRIVATE ${LQL_INTERNAL_LIBRARY})
  lql_configure_c89_target(lql_direct_bench)
endif()

if(LQL_BUILD_BINARY)
  add_executable(clql tools/clql.c)
  target_link_libraries(clql PRIVATE ${LQL_INTERNAL_LIBRARY})
  if(LQL_CLQL_STATIC AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(clql PRIVATE -static)
  endif()
  lql_configure_c89_target(clql)
endif()

if(LQL_BUILD_FUZZERS)
  add_executable(lql_json_fuzz fuzz/json_fuzz.c)
  target_link_libraries(lql_json_fuzz PRIVATE ${LQL_INTERNAL_LIBRARY})
  lql_configure_c89_target(lql_json_fuzz)
endif()

if(LQL_BUILD_LUA)
  find_package(PkgConfig REQUIRED)
  if(LQL_LUA_SDK_PREFIX)
    set(ENV{PKG_CONFIG_PATH}
        "${LQL_LUA_SDK_PREFIX}/${CMAKE_INSTALL_LIBDIR}/pkgconfig:$ENV{PKG_CONFIG_PATH}")
  endif()
  pkg_check_modules(LUA55 REQUIRED lua5.5)
  pkg_check_modules(LIBLQL REQUIRED liblql)
  if(NOT LUA55_INCLUDE_DIRS)
    message(FATAL_ERROR "lua5.5 pkg-config did not report include directories")
  endif()
  list(GET LUA55_INCLUDE_DIRS 0 LUA55_INCLUDE_DIR)
  set(LQL_LUA_STAGED_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/lua-host-include")
  file(MAKE_DIRECTORY "${LQL_LUA_STAGED_INCLUDE}/lua5.5")
  file(GLOB LQL_LUA_HEADERS "${LUA55_INCLUDE_DIR}/*.h")
  foreach(header ${LQL_LUA_HEADERS})
    file(COPY "${header}" DESTINATION "${LQL_LUA_STAGED_INCLUDE}/lua5.5")
  endforeach()
  find_file(LQL_LUA_MULTIARCH_HEADER lua5.5-deb-multiarch.h
    PATHS /usr/include /usr/local/include
    PATH_SUFFIXES x86_64-linux-gnu aarch64-linux-gnu arm-linux-gnueabihf
    NO_CMAKE_FIND_ROOT_PATH)
  if(LQL_LUA_MULTIARCH_HEADER)
    file(COPY "${LQL_LUA_MULTIARCH_HEADER}"
         DESTINATION "${LQL_LUA_STAGED_INCLUDE}/lua5.5")
  endif()
  add_library(lql_lua_core MODULE lua/lql_core.c)
  set_target_properties(lql_lua_core PROPERTIES
    PREFIX ""
    OUTPUT_NAME core
    SKIP_BUILD_RPATH TRUE
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lua/lql")
  target_link_directories(lql_lua_core PRIVATE ${LIBLQL_LIBRARY_DIRS})
  target_link_libraries(lql_lua_core PRIVATE ${LIBLQL_LIBRARIES})
  target_include_directories(lql_lua_core SYSTEM PRIVATE
    "${LQL_LUA_STAGED_INCLUDE}/lua5.5")
  target_include_directories(lql_lua_core PRIVATE ${LIBLQL_INCLUDE_DIRS})
  lql_configure_c89_target(lql_lua_core)
endif()

if(LQL_INSTALL)
  install(DIRECTORY include/lql
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/include/lql/version.h"
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/lql")
  install(FILES LICENSE README.md
          DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/liblql")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/liblql.pc"
          DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
  install(FILES
          "${CMAKE_CURRENT_BINARY_DIR}/liblqlConfig.cmake"
          "${CMAKE_CURRENT_BINARY_DIR}/liblqlConfigVersion.cmake"
          DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/liblql")
  if(LQL_BUILD_STATIC)
    install(TARGETS lql_static
            EXPORT liblqlTargets
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  endif()
  if(LQL_BUILD_SHARED)
    install(TARGETS lql_shared
            EXPORT liblqlTargets
            LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  endif()
  install(EXPORT liblqlTargets
          NAMESPACE liblql::
          DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/liblql")
endif()
