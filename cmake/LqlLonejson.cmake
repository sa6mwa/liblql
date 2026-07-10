set(LQL_LONEJSON_VERSION "0.41.0")
option(LQL_LONEJSON_STATIC "Prefer the static lonejson imported target" OFF)
set(LQL_LONEJSON_PROVIDER "sdk" CACHE STRING "lonejson provider: sdk, vendored")

if(LQL_LONEJSON_PROVIDER STREQUAL "vendored")
  add_library(lonejson_vendored STATIC
    "${CMAKE_SOURCE_DIR}/vendor/lonejson/lonejson.c")
  add_library(lonejson::lonejson ALIAS lonejson_vendored)
  add_library(lonejson::lonejson_static ALIAS lonejson_vendored)
  set_target_properties(lonejson_vendored PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 90
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  target_include_directories(lonejson_vendored PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/vendor/lonejson>")
  if(UNIX OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    target_compile_definitions(lonejson_vendored PRIVATE
      _POSIX_C_SOURCE=200809L
      _FILE_OFFSET_BITS=64)
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_compile_definitions(lonejson_vendored PRIVATE _DARWIN_C_SOURCE=1)
  endif()
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(lonejson_vendored PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  return()
endif()

if(NOT LQL_LONEJSON_PROVIDER STREQUAL "sdk")
  message(FATAL_ERROR
    "LQL_LONEJSON_PROVIDER must be 'sdk' or 'vendored', got '${LQL_LONEJSON_PROVIDER}'")
endif()

if(NOT LQL_EXTERNAL_ROOT)
  set(LQL_EXTERNAL_ROOT "${CMAKE_SOURCE_DIR}/.cache/deps/${LQL_TARGET_ID}/lonejson")
endif()

find_package(lonejson ${LQL_LONEJSON_VERSION} CONFIG QUIET PATHS "${LQL_EXTERNAL_ROOT}" NO_DEFAULT_PATH)
if(LQL_LONEJSON_STATIC AND TARGET lonejson::lonejson_static)
  if(TARGET lonejson::lonejson)
    get_target_property(LQL_LONEJSON_STATIC_LOCATION
      lonejson::lonejson_static IMPORTED_LOCATION)
    set_property(TARGET lonejson::lonejson PROPERTY
      IMPORTED_LOCATION "${LQL_LONEJSON_STATIC_LOCATION}")
    unset(LQL_LONEJSON_STATIC_LOCATION)
  else()
    add_library(lonejson::lonejson ALIAS lonejson::lonejson_static)
  endif()
endif()
if(NOT TARGET lonejson::lonejson)
  find_path(LQL_LONEJSON_INCLUDE_DIR lonejson.h PATHS "${LQL_EXTERNAL_ROOT}/include" NO_DEFAULT_PATH)
  if(LQL_LONEJSON_STATIC)
    find_library(LQL_LONEJSON_LIBRARY NAMES liblonejson.a lonejson
      PATHS "${LQL_EXTERNAL_ROOT}/lib" NO_DEFAULT_PATH)
  else()
    find_library(LQL_LONEJSON_LIBRARY NAMES lonejson PATHS "${LQL_EXTERNAL_ROOT}/lib" NO_DEFAULT_PATH)
  endif()
  if(NOT LQL_LONEJSON_INCLUDE_DIR OR NOT LQL_LONEJSON_LIBRARY)
    message(FATAL_ERROR "lonejson ${LQL_LONEJSON_VERSION} not found under LQL_EXTERNAL_ROOT=${LQL_EXTERNAL_ROOT}. Run make deps-debug or set LQL_EXTERNAL_ROOT to a lonejson SDK root.")
  endif()
  add_library(lonejson::lonejson UNKNOWN IMPORTED)
  set_target_properties(lonejson::lonejson PROPERTIES
    IMPORTED_LOCATION "${LQL_LONEJSON_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LQL_LONEJSON_INCLUDE_DIR}")
endif()
