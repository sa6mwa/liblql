#!/usr/bin/env bash
# Sourced by isolated checkout fixtures and their cache-routing tests.
lql_redirect_fixture_caches() {
  local source_root=$1 fixture_root=$2
  local cache_var cache_path cache_kind global_cache
  for cache_var in CPKT_DEPENDENCY_CACHE CPKT_TOOLCHAIN_CACHE; do
    cache_path=${!cache_var:-}
    [[ -n "$cache_path" ]] || continue
    case "$cache_var" in
      CPKT_DEPENDENCY_CACHE) cache_kind=deps ;;
      CPKT_TOOLCHAIN_CACHE) cache_kind=toolchains ;;
    esac
    cmake -DLQL_CACHE_PATH="$cache_path" -DLQL_CACHE_KIND="$cache_kind" \
      -P "$source_root/cmake/LqlCachePaths.cmake" || return
    cache_path=$(realpath -m -- "$cache_path")
    global_cache=
    if [[ -n "${XDG_CACHE_HOME:-${HOME:-}}" ]]; then
      global_cache=$(realpath -m -- "${XDG_CACHE_HOME:-$HOME/.cache}/cpkt/$cache_kind")
    fi
    if [[ -z "$global_cache" || "$cache_path/" != "$global_cache/"* ]]; then
      case "$cache_path/" in
        "$source_root/build/"*|"$source_root/.cache/"*)
          printf -v "$cache_var" '%s/%s' "$fixture_root" "${cache_path#"$source_root/"}"
          export "$cache_var"
          ;;
      esac
    fi
    cmake -DLQL_CACHE_PATH="${!cache_var}" -DLQL_CACHE_KIND="$cache_kind" \
      -P "$fixture_root/cmake/LqlCachePaths.cmake" || return
    printf 'version fixture: %s=%s\n' "$cache_var" "${!cache_var}"
  done
}
