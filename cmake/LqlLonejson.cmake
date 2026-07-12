foreach(lonejson_variant IN ITEMS static shared)
  if(lonejson_variant STREQUAL "static")
    set(lonejson_library_type STATIC)
  else()
    set(lonejson_library_type SHARED)
  endif()
  add_library(lonejson_vendored_${lonejson_variant} ${lonejson_library_type}
    "${CMAKE_SOURCE_DIR}/vendor/lonejson/lonejson.c")
  add_library(lonejson::${lonejson_variant} ALIAS
    lonejson_vendored_${lonejson_variant})
  set_target_properties(lonejson_vendored_${lonejson_variant} PROPERTIES
    OUTPUT_NAME lonejson
    POSITION_INDEPENDENT_CODE ON
    # CMake has no C89 setting; GNU/Clang targets explicitly override it below.
    C_STANDARD 90
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  if(lonejson_variant STREQUAL "shared")
    set_target_properties(lonejson_vendored_shared PROPERTIES SOVERSION 0)
  endif()
  target_include_directories(lonejson_vendored_${lonejson_variant} PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/vendor/lonejson>")
  if(UNIX OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    target_compile_definitions(lonejson_vendored_${lonejson_variant} PRIVATE
      _POSIX_C_SOURCE=200809L
      _FILE_OFFSET_BITS=64)
  endif()
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(lonejson_vendored_${lonejson_variant} PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
endforeach()

# Compatibility alias for iteration-only tests. Product targets choose their
# matching static/shared LoneJSON ABI explicitly below.
add_library(lonejson::lonejson ALIAS lonejson_vendored_static)
