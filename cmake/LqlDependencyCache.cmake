# Resolve the shared dependency archive cache once for this configuration.
#
# liblql's JSON execution is self-contained.  Lua development builds acquire
# their pinned upstream source archive through the helper below, keeping that
# temporary build input in the shared verified cache rather than the checkout.
if(DEFINED CPKT_DEPENDENCY_CACHE)
  if(CPKT_DEPENDENCY_CACHE STREQUAL "")
    message(FATAL_ERROR "CPKT_DEPENDENCY_CACHE was explicitly set but is empty")
  endif()
elseif(DEFINED ENV{CPKT_DEPENDENCY_CACHE} AND
       NOT "$ENV{CPKT_DEPENDENCY_CACHE}" STREQUAL "")
  set(CPKT_DEPENDENCY_CACHE "$ENV{CPKT_DEPENDENCY_CACHE}")
elseif(DEFINED ENV{XDG_CACHE_HOME} AND
       NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
  set(CPKT_DEPENDENCY_CACHE "$ENV{XDG_CACHE_HOME}/c.pkt.systems/deps")
elseif(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
  set(CPKT_DEPENDENCY_CACHE "$ENV{HOME}/.cache/c.pkt.systems/deps")
else()
  message(FATAL_ERROR
    "CPKT_DEPENDENCY_CACHE, XDG_CACHE_HOME, or HOME is required to resolve the shared dependency archive cache")
endif()

file(TO_CMAKE_PATH "${CPKT_DEPENDENCY_CACHE}" CPKT_DEPENDENCY_CACHE)
set(CPKT_DEPENDENCY_CACHE "${CPKT_DEPENDENCY_CACHE}" CACHE PATH
    "Shared verified dependency archive cache" FORCE)

# Download one immutable source archive into the shared cache.  Callers own
# extraction below their disposable build directory; no cache path may reach a
# shipped artifact or exported package metadata.
function(lql_acquire_verified_archive component url sha256 archive_name out_archive)
  set(cache_dir "${CPKT_DEPENDENCY_CACHE}/archives/sha256/${sha256}")
  set(archive "${cache_dir}/${archive_name}")
  set(lock "${CPKT_DEPENDENCY_CACHE}/locks/${sha256}.lock")
  file(MAKE_DIRECTORY "${cache_dir}" "${CPKT_DEPENDENCY_CACHE}/locks")
  file(LOCK "${lock}" GUARD FUNCTION TIMEOUT 600
       RESULT_VARIABLE lock_result)
  if(NOT lock_result EQUAL 0)
    message(FATAL_ERROR
      "${component}: timed out waiting for dependency cache lock ${lock}")
  endif()

  if(EXISTS "${archive}")
    file(SHA256 "${archive}" actual_sha256)
    if(NOT actual_sha256 STREQUAL sha256)
      file(REMOVE "${archive}")
    endif()
  endif()

  if(NOT EXISTS "${archive}")
    set(temporary "${archive}.tmp")
    file(REMOVE "${temporary}")
    set(download_ok FALSE)
    foreach(attempt RANGE 1 3)
      file(DOWNLOAD "${url}" "${temporary}"
           TLS_VERIFY ON
           STATUS download_status
           LOG download_log)
      list(GET download_status 0 download_code)
      if(download_code EQUAL 0)
        file(SHA256 "${temporary}" downloaded_sha256)
        if(downloaded_sha256 STREQUAL sha256)
          set(download_ok TRUE)
          break()
        endif()
        set(download_log
            "downloaded archive checksum mismatch: expected ${sha256}, got ${downloaded_sha256}")
      endif()
      file(REMOVE "${temporary}")
    endforeach()
    if(NOT download_ok)
      message(FATAL_ERROR
        "${component}: could not download verified archive\n"
        "url=${url}\nsha256=${sha256}\ncache=${archive}\n${download_log}")
    endif()
    file(RENAME "${temporary}" "${archive}")
  endif()

  set(${out_archive} "${archive}" PARENT_SCOPE)
endfunction()
