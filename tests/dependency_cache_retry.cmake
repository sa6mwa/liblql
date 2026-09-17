if(NOT DEFINED RETRY_ARCHIVE_URL OR NOT DEFINED RETRY_ARCHIVE_SOURCE)
  message(FATAL_ERROR "RETRY_ARCHIVE_URL and RETRY_ARCHIVE_SOURCE are required")
endif()

get_filename_component(LQL_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${LQL_SOURCE_ROOT}/cmake/LqlDependencyCache.cmake")

file(SHA256 "${RETRY_ARCHIVE_SOURCE}" expected_sha256)
lql_acquire_verified_archive("dependency cache retry fixture" "${RETRY_ARCHIVE_URL}"
                             "${expected_sha256}" "archive.tar.gz" archive)
file(SHA256 "${archive}" actual_sha256)
if(NOT actual_sha256 STREQUAL expected_sha256)
  message(FATAL_ERROR
    "dependency cache retry: got ${actual_sha256}, expected ${expected_sha256}")
endif()
