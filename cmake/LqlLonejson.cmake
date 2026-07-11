add_library(lonejson_vendored STATIC "${CMAKE_SOURCE_DIR}/vendor/lonejson/lonejson.c")
add_library(lonejson::lonejson ALIAS lonejson_vendored)
set_target_properties(lonejson_vendored PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  # CMake has no C89 setting; GNU/Clang targets explicitly override it below.
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
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(lonejson_vendored PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endif()
