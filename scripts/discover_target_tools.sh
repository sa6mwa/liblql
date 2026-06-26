#!/bin/sh
set -eu

usage() {
  printf 'usage: discover_target_tools.sh BUILD_DIR TARGET_ID\n' >&2
}

shell_quote() {
  printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

emit() {
  printf '%s=' "$1"
  shell_quote "$2"
  printf '\n'
}

cache_value() {
  cache=$1
  key=$2
  if [ ! -f "$cache" ]; then
    return
  fi
  sed -n "s/^${key}:[^=]*=//p" "$cache" | sed -n '1p'
}

path_lookup() {
  name=$1
  case "$name" in
    */*)
      if [ -x "$name" ]; then
        printf '%s\n' "$name"
      fi
      ;;
    *)
      command -v "$name" 2>/dev/null || true
      ;;
  esac
}

compiler_dir() {
  cc=$1
  case "$cc" in
    */*) dirname "$cc" ;;
    *) return ;;
  esac
}

compiler_prefix() {
  cc=$1
  base=$(basename "$cc")
  case "$base" in
    *-gcc) printf '%s\n' "${base%-gcc}" ;;
    *-cc) printf '%s\n' "${base%-cc}" ;;
    *-clang) printf '%s\n' "${base%-clang}" ;;
  esac
}

target_prefixes() {
  target_id=$1
  cc=$2
  derived=$(compiler_prefix "$cc" || true)
  if [ -n "$derived" ]; then
    printf '%s\n' "$derived"
  fi
  case "$target_id" in
    x86_64-linux-gnu) printf '%s\n' x86_64-linux-gnu ;;
    x86_64-linux-musl) printf '%s\n' x86_64-linux-musl ;;
    aarch64-linux-gnu) printf '%s\n' aarch64-linux-gnu ;;
    aarch64-linux-musl) printf '%s\n' aarch64-linux-musl ;;
    armhf-linux-gnu) printf '%s\n' arm-linux-gnueabihf armhf-linux-gnu ;;
    armhf-linux-musl) printf '%s\n' arm-linux-musleabihf armhf-linux-musl ;;
    arm64-apple-darwin)
      printf '%s\n' "${CPKT_OSXCROSS_HOST:-arm64-apple-darwin25}" \
        arm64-apple-darwin
      ;;
  esac | awk 'NF && !seen[$0]++'
}

reject_known_host_darwin_tool() {
  target_id=$1
  tool=$2
  path=$3
  case "$target_id:$tool:$path" in
    arm64-apple-darwin:OTOOL:/usr/bin/otool) return 0 ;;
    arm64-apple-darwin:STRIP:/usr/bin/strip) return 0 ;;
    arm64-apple-darwin:INSTALL_NAME_TOOL:/usr/bin/install_name_tool) return 0 ;;
  esac
  return 1
}

select_candidate() {
  target_id=$1
  tool=$2
  candidate=$3
  resolved=$(path_lookup "$candidate")
  if [ -z "$resolved" ]; then
    return
  fi
  if reject_known_host_darwin_tool "$target_id" "$tool" "$resolved"; then
    return
  fi
  printf '%s\n' "$resolved"
}

select_tool() {
  target_id=$1
  tool=$2
  cache_value_candidate=$3
  cc=$4
  suffix=$5
  path_name=$6
  override_var=$7
  value=''

  eval "override=\${$override_var:-}"
  if [ -n "$override" ]; then
    value=$(select_candidate "$target_id" "$tool" "$override")
    if [ -n "$value" ]; then
      printf '%s\n' "$value"
      return
    fi
  fi

  if [ -n "$cache_value_candidate" ]; then
    value=$(select_candidate "$target_id" "$tool" "$cache_value_candidate")
    if [ -n "$value" ]; then
      printf '%s\n' "$value"
      return
    fi
  fi

  cc_dir=$(compiler_dir "$cc" || true)
  if [ -n "$cc_dir" ]; then
    for prefix in $(target_prefixes "$target_id" "$cc"); do
      value=$(select_candidate "$target_id" "$tool" "$cc_dir/$prefix-$suffix")
      if [ -n "$value" ]; then
        printf '%s\n' "$value"
        return
      fi
    done
    value=$(select_candidate "$target_id" "$tool" "$cc_dir/$suffix")
    if [ -n "$value" ]; then
      printf '%s\n' "$value"
      return
    fi
  fi

  value=$(select_candidate "$target_id" "$tool" "$path_name")
  if [ -n "$value" ]; then
    printf '%s\n' "$value"
  fi
}

discover() {
  build_dir=$1
  target_id=$2
  cache="$build_dir/CMakeCache.txt"

  cc_cache=$(cache_value "$cache" CMAKE_C_COMPILER)
  strip_cache=$(cache_value "$cache" CMAKE_STRIP)
  install_name_tool_cache=$(cache_value "$cache" CMAKE_INSTALL_NAME_TOOL)
  otool_cache=$(cache_value "$cache" CPKT_OTOOL)
  if [ -z "$otool_cache" ]; then
    otool_cache=$(cache_value "$cache" CMAKE_OTOOL)
  fi
  readelf_cache=$(cache_value "$cache" CMAKE_READELF)

  cc=$(select_tool "$target_id" CC "$cc_cache" "$cc_cache" cc cc LQL_CC)
  file_tool=$(select_tool "$target_id" FILE "" "$cc" file file LQL_FILE)
  readelf_tool=$(select_tool "$target_id" READELF "$readelf_cache" "$cc" \
    readelf readelf LQL_READELF)
  otool=$(select_tool "$target_id" OTOOL "$otool_cache" "$cc" otool otool \
    LQL_OTOOL)
  strip=$(select_tool "$target_id" STRIP "$strip_cache" "$cc" strip strip \
    LQL_STRIP)
  install_name_tool=$(select_tool "$target_id" INSTALL_NAME_TOOL \
    "$install_name_tool_cache" "$cc" install_name_tool install_name_tool \
    LQL_INSTALL_NAME_TOOL)
  prefix=$(target_prefixes "$target_id" "$cc" | sed -n '1p')

  emit LQL_TOOL_CC "$cc"
  emit LQL_TOOL_FILE "$file_tool"
  emit LQL_TOOL_READELF "$readelf_tool"
  emit LQL_TOOL_OTOOL "$otool"
  emit LQL_TOOL_STRIP "$strip"
  emit LQL_TOOL_INSTALL_NAME_TOOL "$install_name_tool"
  emit LQL_TOOL_TARGET_PREFIX "$prefix"
}

assert_eq() {
  label=$1
  got=$2
  want=$3
  if [ "$got" != "$want" ]; then
    printf 'discover target tools fixture failed: %s\n' "$label" >&2
    printf '  got:  %s\n  want: %s\n' "$got" "$want" >&2
    exit 1
  fi
}

run_fixtures() {
  tmp=${TMPDIR:-/tmp}/lql-discover-tools-$$
  trap 'rm -rf "$tmp"' EXIT HUP INT TERM
  mkdir -p "$tmp/bin" "$tmp/build" "$tmp/path"

  touch "$tmp/bin/aarch64-linux-gnu-gcc" \
    "$tmp/bin/aarch64-linux-gnu-readelf" \
    "$tmp/bin/readelf" \
    "$tmp/bin/cache-readelf" \
    "$tmp/path/readelf" \
    "$tmp/path/file"
  chmod +x "$tmp/bin/"* "$tmp/path/"*

  cat >"$tmp/build/CMakeCache.txt" <<EOF
CMAKE_C_COMPILER:FILEPATH=$tmp/bin/aarch64-linux-gnu-gcc
CMAKE_READELF:FILEPATH=$tmp/bin/cache-readelf
EOF
  eval "$(PATH="$tmp/path:$PATH" discover "$tmp/build" aarch64-linux-gnu)"
  assert_eq "cache readelf wins" "$LQL_TOOL_READELF" "$tmp/bin/cache-readelf"

  cat >"$tmp/build/CMakeCache.txt" <<EOF
CMAKE_C_COMPILER:FILEPATH=$tmp/bin/aarch64-linux-gnu-gcc
EOF
  eval "$(PATH="$tmp/path:$PATH" discover "$tmp/build" aarch64-linux-gnu)"
  assert_eq "target-prefixed sibling readelf" "$LQL_TOOL_READELF" \
    "$tmp/bin/aarch64-linux-gnu-readelf"

  rm -f "$tmp/bin/aarch64-linux-gnu-readelf"
  eval "$(PATH="$tmp/path:$PATH" discover "$tmp/build" aarch64-linux-gnu)"
  assert_eq "unprefixed compiler sibling readelf" "$LQL_TOOL_READELF" \
    "$tmp/bin/readelf"

  rm -f "$tmp/bin/readelf"
  eval "$(PATH="$tmp/path:$PATH" discover "$tmp/build" aarch64-linux-gnu)"
  assert_eq "PATH fallback readelf" "$LQL_TOOL_READELF" "$tmp/path/readelf"

  cat >"$tmp/build/CMakeCache.txt" <<EOF
CMAKE_C_COMPILER:FILEPATH=$tmp/bin/arm64-apple-darwin25-cc
CMAKE_OTOOL:FILEPATH=/usr/bin/otool
EOF
  touch "$tmp/bin/arm64-apple-darwin25-cc"
  chmod +x "$tmp/bin/arm64-apple-darwin25-cc"
  eval "$(PATH="$tmp/path:$PATH" discover "$tmp/build" arm64-apple-darwin)"
  assert_eq "darwin host otool rejected" "$LQL_TOOL_OTOOL" ""
}

case "${1:-}" in
  --fixtures)
    run_fixtures
    ;;
  "")
    usage
    exit 2
    ;;
  *)
    if [ $# -ne 2 ]; then
      usage
      exit 2
    fi
    discover "$1" "$2"
    ;;
esac
