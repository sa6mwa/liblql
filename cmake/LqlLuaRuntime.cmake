# Lua facade development tests run under a Bootlin-built interpreter.  Keep
# Lua's source archive in the shared verified dependency cache and extract it
# only into the disposable CMake build directory.
function(lql_prepare_lua_runtime out_source_dir)
  set(lua_version "5.5.1")
  set(lua_archive_name "lua-${lua_version}.tar.gz")
  set(lua_sha256 "1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce")
  set(lua_url "https://www.lua.org/ftp/${lua_archive_name}")
  lql_acquire_verified_archive("Lua ${lua_version}" "${lua_url}" "${lua_sha256}"
                               "${lua_archive_name}" lua_archive)

  set(lua_source_dir "${CMAKE_CURRENT_BINARY_DIR}/lua-${lua_version}")
  if(NOT EXISTS "${lua_source_dir}/src/lua.h")
    file(REMOVE_RECURSE "${lua_source_dir}")
    file(ARCHIVE_EXTRACT INPUT "${lua_archive}"
         DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
    if(NOT EXISTS "${lua_source_dir}/src/lua.h")
      message(FATAL_ERROR
        "Lua ${lua_version}: unexpected source archive layout in ${lua_archive}")
    endif()
  endif()
  set(${out_source_dir} "${lua_source_dir}" PARENT_SCOPE)
endfunction()
