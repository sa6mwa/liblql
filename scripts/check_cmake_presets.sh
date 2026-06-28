#!/bin/sh
set -eu

check_presets() {
  presets=$1

  if ! command -v jq >/dev/null 2>&1; then
    printf 'cmake presets check: jq is required\n' >&2
    exit 2
  fi
  if [ ! -f "$presets" ]; then
    printf 'cmake presets check: missing CMakePresets.json: %s\n' "$presets" >&2
    exit 2
  fi

  jq_expect "$presets" '.version >= 6' || return 1
  jq_expect "$presets" '
    .configurePresets[] |
    select(.name == "base" and .hidden == true and .generator == "Ninja" and
      .binaryDir == "${sourceDir}/build/${presetName}" and
      .cacheVariables.CMAKE_EXPORT_COMPILE_COMMANDS == "ON" and
      .cacheVariables.LQL_DEPENDENCY_MODE == "bundled")
  ' || return 1

  for preset in debug debug-lua asan bench-release; do
    jq_expect_arg "$presets" name "$preset" \
      '.configurePresets[] | select(.name == $name)' || return 1
    jq_expect_arg "$presets" name "$preset" \
      '.buildPresets[] | select(.name == $name and .configurePreset == $name)' ||
      return 1
  done

  jq_expect "$presets" '
    .configurePresets[] |
    select(.name == "debug" and .inherits == "base" and
      .cacheVariables.CMAKE_BUILD_TYPE == "Debug" and
      .cacheVariables.LQL_TARGET_ID == "x86_64-linux-gnu")
  ' || return 1
  jq_expect "$presets" '
    .configurePresets[] |
    select(.name == "debug-lua" and .inherits == "debug" and
      .cacheVariables.LQL_BUILD_LUA_MODULE == "ON")
  ' || return 1
  jq_expect "$presets" '
    .configurePresets[] |
    select(.name == "asan" and .inherits == "debug" and
      (.cacheVariables.CMAKE_C_FLAGS | contains("-fsanitize=address,undefined")) and
      (.cacheVariables.CMAKE_C_FLAGS | contains("-fno-omit-frame-pointer")))
  ' || return 1
  jq_expect "$presets" '
    .configurePresets[] |
    select(.name == "bench-release" and .inherits == "base" and
      .cacheVariables.CMAKE_BUILD_TYPE == "Release" and
      .cacheVariables.LQL_BUILD_TESTS == "OFF" and
      .cacheVariables.LQL_BUILD_EXAMPLES == "OFF" and
      .cacheVariables.LQL_BUILD_BENCHMARKS == "ON" and
      .cacheVariables.LQL_BUILD_LUA_MODULE == "OFF" and
      .cacheVariables.LQL_TARGET_ID == "x86_64-linux-gnu" and
      .cacheVariables.LQL_TARGET_ARCH == "x86_64" and
      .cacheVariables.LQL_TARGET_OS == "linux" and
      .cacheVariables.LQL_TARGET_LIBC == "gnu")
  ' || return 1

  for preset in debug asan; do
    jq_expect_arg "$presets" name "$preset" \
      '.testPresets[] | select(.name == $name and .configurePreset == $name)' ||
      return 1
  done

  jq_expect "$presets" '
    .configurePresets[] |
    select(.name == "release-base" and .hidden == true and
      .inherits == "base" and .cacheVariables.CMAKE_BUILD_TYPE == "Release" and
      .cacheVariables.LQL_BUILD_TESTS == "OFF" and
      .cacheVariables.LQL_BUILD_EXAMPLES == "OFF" and
      .cacheVariables.LQL_BUILD_BENCHMARKS == "OFF" and
      .cacheVariables.LQL_BUILD_LUA_MODULE == "OFF")
  ' || return 1

  check_release_preset "$presets" x86_64-linux-gnu x86_64 linux gnu ||
    return 1
  check_release_preset "$presets" x86_64-linux-musl x86_64 linux musl ||
    return 1
  check_release_preset "$presets" aarch64-linux-gnu aarch64 linux gnu ||
    return 1
  check_release_preset "$presets" aarch64-linux-musl aarch64 linux musl ||
    return 1
  check_release_preset "$presets" armhf-linux-gnu armhf linux gnu ||
    return 1
  check_release_preset "$presets" armhf-linux-musl armhf linux musl ||
    return 1
  check_release_preset "$presets" arm64-apple-darwin arm64 darwin "" ||
    return 1
}

jq_expect() {
  presets=$1
  expr=$2
  jq -e "$expr" "$presets" >/dev/null || return 1
}

jq_expect_arg() {
  presets=$1
  arg_name=$2
  arg_value=$3
  expr=$4
  jq -e --arg "$arg_name" "$arg_value" "$expr" "$presets" >/dev/null ||
    return 1
}

check_release_preset() {
  presets=$1
  target_id=$2
  arch=$3
  os=$4
  libc=$5
  name="${target_id}-release"

  jq -e --arg name "$name" --arg target "$target_id" --arg arch "$arch" \
    --arg os "$os" --arg libc "$libc" '
      .configurePresets[] |
      select(.name == $name and .inherits == "release-base" and
        .cacheVariables.LQL_TARGET_ID == $target and
        .cacheVariables.LQL_TARGET_ARCH == $arch and
        .cacheVariables.LQL_TARGET_OS == $os and
        .cacheVariables.LQL_TARGET_LIBC == $libc)
    ' "$presets" >/dev/null || return 1
  if [ "$target_id" = "arm64-apple-darwin" ]; then
    jq -e --arg name "$name" '
      .configurePresets[] |
      select(.name == $name and
        .cacheVariables.CMAKE_SYSTEM_NAME == "Darwin" and
        .cacheVariables.CMAKE_SYSTEM_PROCESSOR == "arm64" and
        .cacheVariables.CMAKE_OSX_DEPLOYMENT_TARGET == "15.0")
    ' "$presets" >/dev/null || return 1
  fi

  jq -e --arg name "$name" \
    '.buildPresets[] | select(.name == $name and .configurePreset == $name)' \
    "$presets" >/dev/null || return 1
}

if [ "${1:-}" = "--fixtures" ]; then
  tmp=${TMPDIR:-/tmp}/liblql-cmake-presets-fixtures.$$
  trap 'rm -rf "$tmp"' EXIT HUP INT TERM
  mkdir -p "$tmp"
  clean="$tmp/CMakePresets.json"
  bad="$tmp/bad.json"

  cat >"$clean" <<'EOF'
{
  "version": 6,
  "configurePresets": [
    {"name":"base","hidden":true,"generator":"Ninja","binaryDir":"${sourceDir}/build/${presetName}","cacheVariables":{"CMAKE_EXPORT_COMPILE_COMMANDS":"ON","LQL_DEPENDENCY_MODE":"bundled"}},
    {"name":"debug","inherits":"base","cacheVariables":{"CMAKE_BUILD_TYPE":"Debug","LQL_TARGET_ID":"x86_64-linux-gnu"}},
    {"name":"debug-lua","inherits":"debug","cacheVariables":{"LQL_BUILD_LUA_MODULE":"ON"}},
    {"name":"asan","inherits":"debug","cacheVariables":{"CMAKE_C_FLAGS":"-fsanitize=address,undefined -fno-omit-frame-pointer"}},
    {"name":"bench-release","inherits":"base","cacheVariables":{"CMAKE_BUILD_TYPE":"Release","LQL_BUILD_TESTS":"OFF","LQL_BUILD_EXAMPLES":"OFF","LQL_BUILD_BENCHMARKS":"ON","LQL_BUILD_LUA_MODULE":"OFF","LQL_TARGET_ID":"x86_64-linux-gnu","LQL_TARGET_ARCH":"x86_64","LQL_TARGET_OS":"linux","LQL_TARGET_LIBC":"gnu"}},
    {"name":"release-base","hidden":true,"inherits":"base","cacheVariables":{"CMAKE_BUILD_TYPE":"Release","LQL_BUILD_TESTS":"OFF","LQL_BUILD_EXAMPLES":"OFF","LQL_BUILD_BENCHMARKS":"OFF","LQL_BUILD_LUA_MODULE":"OFF"}},
    {"name":"x86_64-linux-gnu-release","inherits":"release-base","cacheVariables":{"LQL_TARGET_ID":"x86_64-linux-gnu","LQL_TARGET_ARCH":"x86_64","LQL_TARGET_OS":"linux","LQL_TARGET_LIBC":"gnu"}},
    {"name":"x86_64-linux-musl-release","inherits":"release-base","cacheVariables":{"LQL_TARGET_ID":"x86_64-linux-musl","LQL_TARGET_ARCH":"x86_64","LQL_TARGET_OS":"linux","LQL_TARGET_LIBC":"musl"}},
    {"name":"aarch64-linux-gnu-release","inherits":"release-base","cacheVariables":{"LQL_TARGET_ID":"aarch64-linux-gnu","LQL_TARGET_ARCH":"aarch64","LQL_TARGET_OS":"linux","LQL_TARGET_LIBC":"gnu"}},
    {"name":"aarch64-linux-musl-release","inherits":"release-base","cacheVariables":{"LQL_TARGET_ID":"aarch64-linux-musl","LQL_TARGET_ARCH":"aarch64","LQL_TARGET_OS":"linux","LQL_TARGET_LIBC":"musl"}},
    {"name":"armhf-linux-gnu-release","inherits":"release-base","cacheVariables":{"LQL_TARGET_ID":"armhf-linux-gnu","LQL_TARGET_ARCH":"armhf","LQL_TARGET_OS":"linux","LQL_TARGET_LIBC":"gnu"}},
    {"name":"armhf-linux-musl-release","inherits":"release-base","cacheVariables":{"LQL_TARGET_ID":"armhf-linux-musl","LQL_TARGET_ARCH":"armhf","LQL_TARGET_OS":"linux","LQL_TARGET_LIBC":"musl"}},
    {"name":"arm64-apple-darwin-release","inherits":"release-base","cacheVariables":{"LQL_TARGET_ID":"arm64-apple-darwin","LQL_TARGET_ARCH":"arm64","LQL_TARGET_OS":"darwin","LQL_TARGET_LIBC":"","CMAKE_SYSTEM_NAME":"Darwin","CMAKE_SYSTEM_PROCESSOR":"arm64","CMAKE_OSX_DEPLOYMENT_TARGET":"15.0"}}
  ],
  "buildPresets": [
    {"name":"debug","configurePreset":"debug"},
    {"name":"debug-lua","configurePreset":"debug-lua"},
    {"name":"asan","configurePreset":"asan"},
    {"name":"bench-release","configurePreset":"bench-release"},
    {"name":"x86_64-linux-gnu-release","configurePreset":"x86_64-linux-gnu-release"},
    {"name":"x86_64-linux-musl-release","configurePreset":"x86_64-linux-musl-release"},
    {"name":"aarch64-linux-gnu-release","configurePreset":"aarch64-linux-gnu-release"},
    {"name":"aarch64-linux-musl-release","configurePreset":"aarch64-linux-musl-release"},
    {"name":"armhf-linux-gnu-release","configurePreset":"armhf-linux-gnu-release"},
    {"name":"armhf-linux-musl-release","configurePreset":"armhf-linux-musl-release"},
    {"name":"arm64-apple-darwin-release","configurePreset":"arm64-apple-darwin-release"}
  ],
  "testPresets": [
    {"name":"debug","configurePreset":"debug"},
    {"name":"asan","configurePreset":"asan"}
  ]
}
EOF

  check_presets "$clean"

  jq '.configurePresets |= map(select(.name != "debug-lua"))' \
    "$clean" >"$bad"
  if check_presets "$bad" >/dev/null 2>&1; then
    printf 'cmake presets fixture: expected missing debug-lua preset to fail\n' >&2
    exit 1
  fi

  jq '(.configurePresets[] | select(.name == "base") |
      .cacheVariables.LQL_DEPENDENCY_MODE) = "host"' "$clean" >"$bad"
  if check_presets "$bad" >/dev/null 2>&1; then
    printf 'cmake presets fixture: expected wrong dependency mode to fail\n' >&2
    exit 1
  fi

  jq 'del(.configurePresets[] | select(.name == "release-base") |
      .cacheVariables.LQL_BUILD_BENCHMARKS)' "$clean" >"$bad"
  if check_presets "$bad" >/dev/null 2>&1; then
    printf 'cmake presets fixture: expected missing release benchmark disable to fail\n' >&2
    exit 1
  fi

  jq '(.configurePresets[] | select(.name == "bench-release") |
      .cacheVariables.LQL_BUILD_BENCHMARKS) = "OFF"' "$clean" >"$bad"
  if check_presets "$bad" >/dev/null 2>&1; then
    printf 'cmake presets fixture: expected disabled benchmark release preset to fail\n' >&2
    exit 1
  fi

  jq '(.configurePresets[] | select(.name == "aarch64-linux-musl-release") |
      .cacheVariables.LQL_TARGET_LIBC) = "gnu"' "$clean" >"$bad"
  if check_presets "$bad" >/dev/null 2>&1; then
    printf 'cmake presets fixture: expected wrong release target libc to fail\n' >&2
    exit 1
  fi

  jq '.buildPresets |= map(select(.name != "armhf-linux-musl-release"))' \
    "$clean" >"$bad"
  if check_presets "$bad" >/dev/null 2>&1; then
    printf 'cmake presets fixture: expected missing release build preset to fail\n' >&2
    exit 1
  fi

  exit 0
fi

if [ "$#" -ne 1 ]; then
  printf 'usage: %s CMakePresets.json\n' "$0" >&2
  exit 2
fi

check_presets "$1"
