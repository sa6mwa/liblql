# Resolve the shared dependency archive cache once for this configuration.
#
# liblql v0 currently has no external archive dependencies: JSON execution is
# self-contained and Lua consumes the installed liblql SDK.  Consequently this
# module deliberately does not create the directory or acquire an archive.
# Keeping the resolved cache variable in the configure contract means that a
# future checksum-pinned dependency can use one verified shared cache rather
# than inventing a project-local download path.
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
