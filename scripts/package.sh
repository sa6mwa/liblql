#!/bin/sh
set -eu

PROJECT=liblql
CLI_PROJECT=clql
TARGET=${1:-package}
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DIST_DIR=${LQL_DIST_DIR:-"$ROOT_DIR/dist"}
TARGETS=${LQL_PACKAGE_TARGETS:-x86_64-linux-gnu}
MATRIX_TARGETS="x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl armhf-linux-gnu armhf-linux-musl arm64-apple-darwin"
MATRIX_MODE=0

version() {
  (cd "$ROOT_DIR" && ./scripts/release_version.sh)
}

manifest_path() {
  printf '%s/%s-%s-CHECKSUMS\n' "$DIST_DIR" "$PROJECT" "$(version)"
}

read_lonejson_stamp() {
  target_id=$1
  stamp="$ROOT_DIR/.cache/deps/$target_id/lonejson/.lql-dep-stamp"
  dep_name=
  dep_version=
  dep_target=
  dep_sha=
  if [ ! -f "$stamp" ]; then
    printf 'package: lonejson dependency stamp missing for %s: %s\n' "$target_id" "$stamp" >&2
    exit 1
  fi
  set -- $(sed -n '1p' "$stamp")
  dep_name=${1:-}
  dep_version=${2:-}
  dep_target=${3:-}
  dep_sha=${4:-}
  if [ "$dep_name" != "lonejson" ] || [ "$dep_target" != "$target_id" ] || [ -z "$dep_sha" ]; then
    printf 'package: invalid lonejson dependency stamp for %s: %s\n' "$target_id" "$stamp" >&2
    exit 1
  fi
  LONEJSON_DEP_VERSION=$dep_version
  LONEJSON_DEP_SHA256=$dep_sha
  LONEJSON_DEP_ARCHIVE="liblonejson-${LONEJSON_DEP_VERSION}-${target_id}.tar.gz"
  LONEJSON_DEP_URL="https://github.com/sa6mwa/lonejson/releases/download/v${LONEJSON_DEP_VERSION}/${LONEJSON_DEP_ARCHIVE}"
}

write_dependency_manifest() {
  out=$1
  target_id=$2
  bundled=$3
  role=$4

  read_lonejson_stamp "$target_id"
  mkdir -p "$(dirname "$out")"
  cat >"$out" <<EOF
{
  "schema": "liblql.dependencies.v1",
  "target": "$target_id",
  "dependencies": [
    {
      "name": "lonejson",
      "version": "$LONEJSON_DEP_VERSION",
      "target": "$target_id",
      "source": "github-release",
      "source_url": "$LONEJSON_DEP_URL",
      "archive": "$LONEJSON_DEP_ARCHIVE",
      "sha256": "$LONEJSON_DEP_SHA256",
      "license": "MIT",
      "bundled": $bundled,
      "role": "$role"
    }
  ]
}
EOF
}

clean_dist() {
  mkdir -p "$DIST_DIR"
  find "$DIST_DIR" -maxdepth 1 \( \
    -name "${PROJECT}-*.tar.gz" -o \
    -name "${PROJECT}-lua-*.tar.gz" -o \
    -name "${PROJECT}-*.rockspec" -o \
    -name "${PROJECT}-*.src.rock" -o \
    -name "${CLI_PROJECT}-*.tar.gz" -o \
    -name "${PROJECT}-*-CHECKSUMS" \) -exec rm -f {} +
}

make_tar_gz() {
  src_root=$1
  out=$2
  parent=$(dirname "$src_root")
  base=$(basename "$src_root")
  if tar --version 2>/dev/null | grep -qi 'gnu tar'; then
    (cd "$parent" && tar --sort=name --mtime='UTC 1970-01-01' \
      --owner=0 --group=0 --numeric-owner -czf "$out" "$base")
  else
    (cd "$parent" && tar -czf "$out" "$base")
  fi
}

package_one() {
  target_id=$1
  preset="${target_id}-release"
  build_dir="$ROOT_DIR/build/$preset"
  version_value=$(version)
  work_dir="$ROOT_DIR/build/package/$target_id"
  install_root="$work_dir/install"
  lib_root="$work_dir/${PROJECT}-${version_value}-${target_id}"
  cli_root="$work_dir/${CLI_PROJECT}-${version_value}-${target_id}"
  dep_root="$ROOT_DIR/.cache/deps/$target_id/lonejson"
  cc=$(target_cc "$target_id" || true)

  if [ -z "$cc" ]; then
    if [ "$MATRIX_MODE" = 1 ]; then
      printf 'release-matrix: skipping %s: target compiler unavailable\n' "$target_id" >&2
      return 0
    fi
    printf 'package: target compiler unavailable for %s\n' "$target_id" >&2
    exit 1
  fi

  if ! compiler_link_smoke "$target_id" "$cc"; then
    if [ "$MATRIX_MODE" = 1 ]; then
      printf 'release-matrix: skipping %s: target compiler cannot link\n' "$target_id" >&2
      return 0
    fi
    printf 'package: target compiler cannot link for %s: %s\n' "$target_id" "$cc" >&2
    exit 1
  fi

  "$ROOT_DIR/scripts/deps.sh" "$target_id"
  reset_build_dir_if_compiler_changed "$build_dir" "$cc"
  (cd "$ROOT_DIR" && CC="$cc" cmake --preset "$preset")
  (cd "$ROOT_DIR" && cmake --build --preset "$preset")
  rm -rf "$work_dir"
  mkdir -p "$install_root"
  cmake --install "$build_dir" --prefix "$install_root" --strip

  cp -R "$install_root" "$lib_root"
  rm -rf "$lib_root/bin"
  write_dependency_manifest "$lib_root/share/$PROJECT/dependencies.json" \
    "$target_id" false "external-sdk"

  mkdir -p "$cli_root/bin" "$cli_root/lib" "$cli_root/share/doc/$CLI_PROJECT" \
    "$cli_root/share/doc/lonejson" "$cli_root/share/$CLI_PROJECT"
  cp "$install_root/bin/clql" "$cli_root/bin/clql"
  if ls "$dep_root"/lib/liblonejson.so* >/dev/null 2>&1; then
    cp -P "$dep_root"/lib/liblonejson.so* "$cli_root/lib/"
  fi
  if ls "$dep_root"/lib/liblonejson*.dylib* >/dev/null 2>&1; then
    cp -P "$dep_root"/lib/liblonejson*.dylib* "$cli_root/lib/"
  fi
  cp "$ROOT_DIR/LICENSE" "$cli_root/share/doc/$CLI_PROJECT/LICENSE"
  cp "$ROOT_DIR/README.md" "$cli_root/share/doc/$CLI_PROJECT/README.md"
  if [ -f "$dep_root/share/doc/liblonejson/LICENSE" ]; then
    cp "$dep_root/share/doc/liblonejson/LICENSE" "$cli_root/share/doc/lonejson/LICENSE"
  fi
  write_dependency_manifest "$cli_root/share/$CLI_PROJECT/dependencies.json" \
    "$target_id" true "runtime"

  make_tar_gz "$lib_root" "$DIST_DIR/${PROJECT}-${version_value}-${target_id}.tar.gz"
  make_tar_gz "$cli_root" "$DIST_DIR/${CLI_PROJECT}-${version_value}-${target_id}.tar.gz"
}

target_cc() {
  target_id=$1
  case "$target_id" in
    x86_64-linux-gnu)
      command -v "${CC:-cc}" 2>/dev/null
      ;;
    x86_64-linux-musl)
      command -v "${LQL_CC_X86_64_LINUX_MUSL:-x86_64-linux-musl-gcc}" 2>/dev/null
      ;;
    aarch64-linux-gnu)
      command -v "${LQL_CC_AARCH64_LINUX_GNU:-aarch64-linux-gnu-gcc}" 2>/dev/null
      ;;
    aarch64-linux-musl)
      command -v "${LQL_CC_AARCH64_LINUX_MUSL:-aarch64-linux-musl-gcc}" 2>/dev/null ||
        command -v "$HOME/.local/cross/aarch64-linux-musl/bin/aarch64-linux-musl-gcc" 2>/dev/null
      ;;
    armhf-linux-gnu)
      command -v "${LQL_CC_ARMHF_LINUX_GNU:-arm-linux-gnueabihf-gcc}" 2>/dev/null ||
        command -v armhf-linux-gnu-gcc 2>/dev/null
      ;;
    armhf-linux-musl)
      command -v "${LQL_CC_ARMHF_LINUX_MUSL:-arm-linux-musleabihf-gcc}" 2>/dev/null ||
        command -v armhf-linux-musl-gcc 2>/dev/null ||
        command -v "$HOME/.local/cross/arm-linux-musleabihf/bin/arm-linux-musleabihf-gcc" 2>/dev/null
      ;;
    arm64-apple-darwin)
      darwin_host=${CPKT_OSXCROSS_HOST:-arm64-apple-darwin25}
      command -v "${LQL_CC_ARM64_APPLE_DARWIN:-$darwin_host-cc}" 2>/dev/null ||
        command -v "$darwin_host-clang" 2>/dev/null ||
        command -v "${OSXCROSS_ROOT:-$HOME/.local/cross/osxcross}/bin/$darwin_host-cc" 2>/dev/null ||
        command -v "${OSXCROSS_ROOT:-$HOME/.local/cross/osxcross}/bin/$darwin_host-clang" 2>/dev/null
      ;;
    *)
      return 1
      ;;
  esac
}

reset_build_dir_if_compiler_changed() {
  build_dir=$1
  cc=$2
  cache="$build_dir/CMakeCache.txt"
  if [ ! -f "$cache" ]; then
    return
  fi
  cached=$(sed -n 's/^CMAKE_C_COMPILER:FILEPATH=//p' "$cache")
  if [ -n "$cached" ] && [ "$cached" != "$cc" ]; then
    rm -rf "$build_dir"
  fi
}

compiler_link_smoke() {
  target_id=$1
  cc=$2
  smoke_dir="$ROOT_DIR/build/package-compiler-smoke/$target_id"
  rm -rf "$smoke_dir"
  mkdir -p "$smoke_dir"
  cat >"$smoke_dir/smoke.c" <<'EOF'
int main(void) {
  return 0;
}
EOF
  "$cc" "$smoke_dir/smoke.c" -o "$smoke_dir/smoke" >/dev/null 2>&1
}

package_source() {
  version_value=$(version)
  work_dir="$ROOT_DIR/build/package/source"
  source_root="$work_dir/${PROJECT}-${version_value}"
  manifest="$work_dir/source-files.txt"

  rm -rf "$work_dir"
  mkdir -p "$source_root"
  if [ -d "$ROOT_DIR/.git" ]; then
    (cd "$ROOT_DIR" && git ls-files) >"$manifest"
  elif [ -f "$ROOT_DIR/RELEASE_MANIFEST" ]; then
    sed '/^VERSION$/d;/^RELEASE_MANIFEST$/d' "$ROOT_DIR/RELEASE_MANIFEST" >"$manifest"
  else
    printf 'package-source: git metadata or RELEASE_MANIFEST is required\n' >&2
    exit 1
  fi
  (cd "$ROOT_DIR" && tar -cf - -T "$manifest") | (cd "$source_root" && tar -xf -)
  printf '%s\n' "$version_value" >"$source_root/VERSION"
  {
    cat "$manifest"
    printf '%s\n' VERSION RELEASE_MANIFEST
  } | LC_ALL=C sort >"$source_root/RELEASE_MANIFEST"
  make_tar_gz "$source_root" "$DIST_DIR/${PROJECT}-${version_value}.tar.gz"
}

package_lua_source() {
  version_value=$(version)
  work_dir="$ROOT_DIR/build/package/lua-source"
  source_root="$work_dir/${PROJECT}-lua-${version_value}"

  rm -rf "$work_dir"
  "$ROOT_DIR/scripts/stage_lua_rock_sources.sh" "$source_root" "$version_value"
  make_tar_gz "$source_root" "$DIST_DIR/${PROJECT}-lua-${version_value}.tar.gz"
}

package_lua_rockspec() {
  version_value=$(version)
  "$ROOT_DIR/scripts/render_release_rockspec.sh" \
    "$version_value" "$DIST_DIR/${PROJECT}-${version_value}-1.rockspec"
}

package_lua_source_rock() {
  version_value=$(version)
  source_tar="$DIST_DIR/${PROJECT}-lua-${version_value}.tar.gz"
  release_rockspec="$DIST_DIR/${PROJECT}-${version_value}-1.rockspec"
  work_dir="$ROOT_DIR/build/package/lua-rock"
  pack_rockspec="$work_dir/${PROJECT}-${version_value}-1.rockspec"
  packed_rock="$work_dir/${PROJECT}-${version_value}-1.src.rock"
  unpack_dir="$work_dir/unpack"
  out_rock="$DIST_DIR/${PROJECT}-${version_value}-1.src.rock"

  if ! command -v luarocks >/dev/null 2>&1; then
    printf 'release-lua-artifacts: luarocks executable not found\n' >&2
    exit 1
  fi
  test -f "$source_tar"
  test -f "$release_rockspec"
  rm -rf "$work_dir"
  mkdir -p "$work_dir" "$unpack_dir"

  LQL_ALLOW_LOCAL_LUA_SOURCE_URL=1 \
  LQL_LUA_SOURCE_URL="file://$source_tar" \
    "$ROOT_DIR/scripts/render_release_rockspec.sh" "$version_value" "$pack_rockspec"
  (cd "$work_dir" && luarocks pack "$pack_rockspec" >/dev/null)

  unzip -q "$packed_rock" -d "$unpack_dir"
  cp "$release_rockspec" "$unpack_dir/${PROJECT}-${version_value}-1.rockspec"
  test -f "$unpack_dir/${PROJECT}-lua-${version_value}.tar.gz"
  rm -f "$out_rock"
  (cd "$unpack_dir" && zip -X -q "$out_rock" \
    "${PROJECT}-${version_value}-1.rockspec" \
    "${PROJECT}-lua-${version_value}.tar.gz")
}

package_all() {
  clean_dist
  for target_id in $TARGETS; do
    package_one "$target_id"
  done
  package_source
  package_lua_source
  package_lua_rockspec
  package_lua_source_rock
  write_checksums
}

write_checksums() {
  version_value=$(version)
  manifest=$(manifest_path)
  tmp="${manifest}.tmp"
  rm -f "$tmp"
  (
    cd "$DIST_DIR"
    for artifact in "${PROJECT}-${version_value}.tar.gz" "${PROJECT}-lua-${version_value}.tar.gz" "${PROJECT}-${version_value}-1.rockspec" "${PROJECT}-${version_value}-1.src.rock" "${PROJECT}-${version_value}"-*.tar.gz "${CLI_PROJECT}-${version_value}"-*.tar.gz; do
      [ -e "$artifact" ] || continue
      sha256sum "$artifact"
    done
  ) >"$tmp"
  if [ ! -s "$tmp" ]; then
    printf 'package: no artifacts found for checksums\n' >&2
    rm -f "$tmp"
    exit 1
  fi
  mv "$tmp" "$manifest"
}

verify_no_local_paths() {
  artifact=$1
  extracted=$2
  for needle in "$ROOT_DIR" "$HOME" "$ROOT_DIR/build" "$ROOT_DIR/.cache" "file://$ROOT_DIR" "file://$HOME"; do
    [ -n "$needle" ] || continue
    if grep -R -I -a -F "$needle" "$extracted" >/tmp/lql-package-grep.$$ 2>/dev/null; then
      printf 'package-verify: local path leak in %s: %s\n' "$artifact" "$needle" >&2
      sed -n '1p' /tmp/lql-package-grep.$$ >&2
      rm -f /tmp/lql-package-grep.$$
      exit 1
    fi
  done
  rm -f /tmp/lql-package-grep.$$
}

verify_elf_runtime_paths() {
  artifact=$1
  root=$2
  failed=0
  if ! command -v readelf >/dev/null 2>&1; then
    return
  fi
  find "$root" -type f >"/tmp/lql-package-files.$$"
  while IFS= read -r file; do
    if ! readelf -h "$file" >/dev/null 2>&1; then
      continue
    fi
    if readelf -d "$file" 2>/dev/null | grep -E 'RPATH|RUNPATH' >/tmp/lql-readelf.$$; then
      if grep -E '/home|/tmp|/var/tmp|/Users|/workspace|/build|\.cache' /tmp/lql-readelf.$$ >/dev/null; then
        printf 'package-verify: non-relocatable runtime path in %s: %s\n' "$artifact" "$file" >&2
        cat /tmp/lql-readelf.$$ >&2
        failed=1
        break
      fi
    fi
  done <"/tmp/lql-package-files.$$"
  rm -f /tmp/lql-readelf.$$ "/tmp/lql-package-files.$$"
  if [ "$failed" != "0" ]; then
    exit 1
  fi
}

verify_target_file() {
  artifact=$1
  target_id=$2
  file_path=$3
  if ! command -v file >/dev/null 2>&1; then
    printf 'package-verify: file(1) unavailable; skipping target file check for %s\n' "$file_path"
    return
  fi
  desc=$(file -L "$file_path")
  case "$target_id" in
    x86_64-linux-gnu|x86_64-linux-musl)
      expected='x86-64'
      ;;
    aarch64-linux-gnu|aarch64-linux-musl)
      expected='ARM aarch64'
      ;;
    armhf-linux-gnu|armhf-linux-musl)
      expected='ARM'
      ;;
    arm64-apple-darwin)
      expected='Mach-O 64-bit arm64'
      ;;
    *)
      expected=''
      ;;
  esac
  if [ -n "$expected" ] && ! printf '%s\n' "$desc" | grep -F "$expected" >/dev/null; then
    printf 'package-verify: target architecture mismatch in %s\n' "$artifact" >&2
    printf '  target=%s\n  file=%s\n  got=%s\n  want-substring=%s\n' \
      "$target_id" "$file_path" "$desc" "$expected" >&2
    exit 1
  fi
}

verify_dependency_manifest() {
  artifact=$1
  root=$2
  package_name=$3
  target_id=$4
  bundled=$5
  role=$6
  dep_manifest="$root/share/$package_name/dependencies.json"

  test -f "$dep_manifest"
  grep -qx '{' "$dep_manifest"
  grep -q '"schema": "liblql.dependencies.v1"' "$dep_manifest"
  grep -q "\"target\": \"$target_id\"" "$dep_manifest"
  grep -q '"name": "lonejson"' "$dep_manifest"
  grep -q "\"target\": \"$target_id\"" "$dep_manifest"
  grep -q '"source": "github-release"' "$dep_manifest"
  grep -q '"source_url": "https://github.com/sa6mwa/lonejson/releases/download/v' "$dep_manifest"
  grep -q "\"archive\": \"liblonejson-.*-${target_id}.tar.gz\"" "$dep_manifest"
  grep -Eq '"sha256": "[0-9a-f]{64}"' "$dep_manifest"
  grep -q '"license": "MIT"' "$dep_manifest"
  grep -q "\"bundled\": $bundled" "$dep_manifest"
  grep -q "\"role\": \"$role\"" "$dep_manifest"
  verify_no_local_paths "$artifact" "$dep_manifest"

  if [ "$package_name" = "$PROJECT" ]; then
    grep -q 'find_dependency(lonejson CONFIG)' "$root/lib/cmake/liblql/liblqlConfig.cmake"
    grep -q 'Requires.private: lonejson' "$root/lib/pkgconfig/liblql.pc"
  fi
  if [ "$bundled" = true ]; then
    test -f "$root/share/doc/lonejson/LICENSE"
  fi
}

is_host_smoke_target() {
  target_id=$1
  machine=$(uname -m 2>/dev/null || printf unknown)
  system=$(uname -s 2>/dev/null || printf unknown)
  case "$system:$machine:$target_id" in
    Linux:x86_64:x86_64-linux-gnu) return 0 ;;
    Darwin:arm64:arm64-apple-darwin) return 0 ;;
    *) return 1 ;;
  esac
}

write_consumer_source() {
  out=$1
  cat >"$out" <<'EOF'
#include <lql/lql.h>
#include <lql/version.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  const char *expr = "contains{f=/status,v=open}";
  lql *ctx = NULL;
  lql_selector *selector = NULL;
  lql_error error;
  lql_status status;
  lql_error_init(&error);
  status = lql_new(&ctx, &error);
  if (status != LQL_STATUS_OK) {
    fprintf(stderr, "create failed: %s\n", error.message);
    return 1;
  }
  status = ctx->selector_parse(ctx, expr, &selector, &error);
  if (status != LQL_STATUS_OK) {
    fprintf(stderr, "parse failed: %s\n", error.message);
    ctx->destroy(ctx);
    return 1;
  }
  if (strcmp(lql_version(), LQL_VERSION) != 0) {
    fprintf(stderr, "version mismatch: %s != %s\n", lql_version(), LQL_VERSION);
    return 1;
  }
  {
    lql_capabilities caps;
    lql_capabilities_get(&caps);
    if (!caps.selector_parse || !caps.source_decision_stream ||
        !caps.source_spooled_match_stream || !caps.spooled_payloads ||
        !caps.payload_sink_write ||
        !caps.projection_source || !caps.projection_buffered_json ||
        !caps.compact_source ||
        !caps.mutation_file_range || !caps.mutation_source ||
        !caps.mutation_buffered_json) {
      fprintf(stderr, "capability query mismatch\n");
      ctx->selector_destroy(ctx, selector);
      ctx->destroy(ctx);
      return 1;
    }
  }
  ctx->selector_destroy(ctx, selector);
  ctx->destroy(ctx);
  return 0;
}
EOF
}

verify_host_consumers() {
  artifact=$1
  root=$2
  target_id=$3
  dep_root="$ROOT_DIR/.cache/deps/$target_id/lonejson"
  cc=${CC:-cc}
  smoke_dir="$ROOT_DIR/build/package-verify-consumer/$target_id"

  if ! is_host_smoke_target "$target_id"; then
    printf 'package-verify: skipping host consumer smoke for %s\n' "$target_id"
    return
  fi
  if [ ! -d "$dep_root" ]; then
    printf 'package-verify: missing lonejson SDK for consumer smoke: %s\n' "$dep_root" >&2
    exit 1
  fi
  if ! command -v "$cc" >/dev/null 2>&1; then
    printf 'package-verify: C compiler unavailable for consumer smoke: %s\n' "$cc" >&2
    exit 1
  fi

  rm -rf "$smoke_dir"
  mkdir -p "$smoke_dir/direct" "$smoke_dir/cmake" "$smoke_dir/pkgconfig"
  write_consumer_source "$smoke_dir/consumer.c"

  "$cc" -std=c90 -Wall -Wextra -Wpedantic -Werror \
    -I"$root/include" -I"$dep_root/include" \
    "$smoke_dir/consumer.c" "$root/lib/liblql.a" "$dep_root/lib/liblonejson.a" \
    -o "$smoke_dir/direct/consumer"
  "$smoke_dir/direct/consumer"

  cat >"$smoke_dir/cmake/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(liblql_package_consumer C)
set(CMAKE_C_STANDARD 90)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT DEFINED LQL_CONSUMER_TARGET)
  set(LQL_CONSUMER_TARGET liblql::lql_static)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()
find_package(liblql CONFIG REQUIRED)
add_executable(consumer ../consumer.c)
target_link_libraries(consumer PRIVATE ${LQL_CONSUMER_TARGET})
EOF
  cmake -S "$smoke_dir/cmake" -B "$smoke_dir/cmake-static-build" \
    -DCMAKE_PREFIX_PATH="$root;$dep_root" >/dev/null
  cmake --build "$smoke_dir/cmake-static-build" >/dev/null
  "$smoke_dir/cmake-static-build/consumer"

  if [ -f "$root/lib/liblql.so" ] || [ -f "$root/lib/liblql.dylib" ]; then
    cmake -S "$smoke_dir/cmake" -B "$smoke_dir/cmake-shared-build" \
      -DCMAKE_PREFIX_PATH="$root;$dep_root" \
      -DLQL_CONSUMER_TARGET=liblql::lql_shared >/dev/null
    cmake --build "$smoke_dir/cmake-shared-build" >/dev/null
    LD_LIBRARY_PATH="$root/lib:$dep_root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
      DYLD_LIBRARY_PATH="$root/lib:$dep_root/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
      "$smoke_dir/cmake-shared-build/consumer"
  fi

  if command -v pkg-config >/dev/null 2>&1; then
    PKG_CONFIG_PATH="$root/lib/pkgconfig:$dep_root/lib/pkgconfig" \
      "$cc" -std=c90 -Wall -Wextra -Wpedantic -Werror \
      "$smoke_dir/consumer.c" \
      $(PKG_CONFIG_PATH="$root/lib/pkgconfig:$dep_root/lib/pkgconfig" pkg-config --cflags --libs --static liblql) \
      -o "$smoke_dir/pkgconfig/consumer"
    LD_LIBRARY_PATH="$root/lib:$dep_root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
      "$smoke_dir/pkgconfig/consumer"
    if [ -f "$root/lib/liblql.so" ] || [ -f "$root/lib/liblql.dylib" ]; then
      PKG_CONFIG_PATH="$root/lib/pkgconfig:$dep_root/lib/pkgconfig" \
        "$cc" -std=c90 -Wall -Wextra -Wpedantic -Werror \
        "$smoke_dir/consumer.c" \
        $(PKG_CONFIG_PATH="$root/lib/pkgconfig:$dep_root/lib/pkgconfig" pkg-config --cflags --libs liblql) \
        -o "$smoke_dir/pkgconfig/consumer-shared"
      LD_LIBRARY_PATH="$root/lib:$dep_root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        DYLD_LIBRARY_PATH="$root/lib:$dep_root/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
        "$smoke_dir/pkgconfig/consumer-shared"
    fi
  else
    printf 'package-verify: pkg-config unavailable; skipping pkg-config consumer smoke for %s\n' "$artifact"
  fi
}

write_current_source_manifest() {
  output=$1
  if [ ! -d "$ROOT_DIR/.git" ]; then
    return 1
  fi
  {
    (cd "$ROOT_DIR" && git ls-files)
    printf '%s\n' VERSION RELEASE_MANIFEST
  } | LC_ALL=C sort >"$output"
}

verify_source_manifest_matches_current() {
  manifest=$1
  artifact=$2
  expected=$3

  if [ ! -d "$ROOT_DIR/.git" ]; then
    return 0
  fi
  write_current_source_manifest "$expected"
  if ! cmp -s "$expected" "$manifest"; then
    printf 'package-verify: source archive manifest does not match current tracked repository files: %s\n' "$artifact" >&2
    diff -u "$expected" "$manifest" >&2 || true
    exit 1
  fi
}

verify_source_archive() {
  artifact=$1
  version_value=$(version)
  tmp_dir="$ROOT_DIR/build/package-source-verify"
  root="$tmp_dir/${PROJECT}-${version_value}"
  dep_root="$ROOT_DIR/.cache/deps/x86_64-linux-gnu/lonejson"

  rm -rf "$tmp_dir"
  mkdir -p "$tmp_dir"
  tar -xzf "$artifact" -C "$tmp_dir"
  if [ ! -d "$root" ]; then
    printf 'package-verify: source archive root missing: %s\n' "$root" >&2
    exit 1
  fi
  test -f "$root/VERSION"
  test -f "$root/RELEASE_MANIFEST"
  if [ "$(sed -n '1p' "$root/VERSION")" != "$version_value" ]; then
    printf 'package-verify: source VERSION mismatch in %s\n' "$artifact" >&2
    exit 1
  fi
  (cd "$root" && find . -type f | sed 's#^\./##' | LC_ALL=C sort) >"$tmp_dir/payload-files.txt"
  if ! cmp -s "$root/RELEASE_MANIFEST" "$tmp_dir/payload-files.txt"; then
    printf 'package-verify: source archive payload does not match RELEASE_MANIFEST\n' >&2
    diff -u "$root/RELEASE_MANIFEST" "$tmp_dir/payload-files.txt" >&2 || true
    exit 1
  fi
  verify_source_manifest_matches_current "$root/RELEASE_MANIFEST" "$artifact" \
    "$tmp_dir/current-source-files.txt"
  if find "$root" \( -name .git -o -name build -o -name dist -o -name .cache \) | grep . >/dev/null; then
    printf 'package-verify: source archive contains generated or VCS state\n' >&2
    exit 1
  fi
  if [ ! -d "$dep_root" ]; then
    "$ROOT_DIR/scripts/deps.sh" x86_64-linux-gnu
  fi
  cmake -S "$root" -B "$tmp_dir/build" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DLQL_TARGET_ID=x86_64-linux-gnu \
    -DLQL_DEPENDENCY_MODE=bundled \
    -DLQL_EXTERNAL_ROOT="$dep_root" >/dev/null
  cmake --build "$tmp_dir/build" >/dev/null
  ctest --test-dir "$tmp_dir/build" --output-on-failure
}

verify_lua_source_archive() {
  artifact=$1
  version_value=$(version)
  tmp_dir="$ROOT_DIR/build/package-lua-source-verify"
  root="$tmp_dir/${PROJECT}-lua-${version_value}"

  rm -rf "$tmp_dir"
  mkdir -p "$tmp_dir"
  tar -xzf "$artifact" -C "$tmp_dir"
  if [ ! -d "$root" ]; then
    printf 'package-verify: Lua source archive root missing: %s\n' "$root" >&2
    exit 1
  fi
  test -f "$root/VERSION"
  test -f "$root/RELEASE_MANIFEST"
  test -f "$root/LICENSE"
  test -f "$root/README.md"
  test -f "$root/lua/lql.lua"
  test -f "$root/lua/lql_core.c"
  test -f "$root/lua/tests/lql_smoke.lua"
  test -f "$root/lua/tests/lql_core_smoke.lua"
  test -f "$root/lua/benchmarks/parity.lua"
  test -f "$root/liblql.rockspec.in"
  test -f "$root/scripts/build_lua_rock.sh"
  test -f "$root/scripts/check_lua_runtime_fixtures.sh"
  test -f "$root/scripts/release_version.sh"
  test -f "$root/scripts/render_release_rockspec.sh"
  test -f "$root/scripts/run_lua_tests.sh"
  test -f "$root/scripts/stage_lua_rock_sources.sh"
  if [ "$(sed -n '1p' "$root/VERSION")" != "$version_value" ]; then
    printf 'package-verify: Lua source VERSION mismatch in %s\n' "$artifact" >&2
    exit 1
  fi
  if ! grep -qx '#if LUA_VERSION_NUM != 505' "$root/lua/lql_core.c"; then
    printf 'package-verify: Lua C module must enforce Lua 5.5 only in %s\n' "$artifact" >&2
    exit 1
  fi
  if ! grep -Fqx '   "lua >= 5.5, < 5.6"' "$root/liblql.rockspec.in"; then
    printf 'package-verify: Lua source rockspec template must require Lua 5.5 only in %s\n' "$artifact" >&2
    exit 1
  fi
  (cd "$root" && find . -type f | sed 's#^\./##' | LC_ALL=C sort) >"$tmp_dir/payload-files.txt"
  if ! cmp -s "$root/RELEASE_MANIFEST" "$tmp_dir/payload-files.txt"; then
    printf 'package-verify: Lua source archive payload does not match RELEASE_MANIFEST\n' >&2
    diff -u "$root/RELEASE_MANIFEST" "$tmp_dir/payload-files.txt" >&2 || true
    exit 1
  fi
  if find "$root" \( -name .git -o -name build -o -name dist -o -name .cache -o -name bin -o -name lib -o -name include \) | grep . >/dev/null; then
    printf 'package-verify: Lua source archive contains generated state or C SDK payloads\n' >&2
    exit 1
  fi
}

verify_one_rockspec() {
  artifact=$1
  version_value=$(version)

  test -f "$artifact"
  grep -qx 'package = "liblql"' "$artifact"
  grep -qx "version = \"${version_value}-1\"" "$artifact"
  grep -q "url = \"https://github.com/sa6mwa/liblql/releases/download/v${version_value}/${PROJECT}-lua-${version_value}.tar.gz\"" "$artifact"
  grep -qx "   dir = \"${PROJECT}-lua-${version_value}\"" "$artifact"
  if ! grep -Fqx '   "lua >= 5.5, < 5.6"' "$artifact"; then
    printf 'package-verify: release rockspec must require Lua 5.5 only: %s\n' "$artifact" >&2
    exit 1
  fi
  if grep -q 'file://' "$artifact"; then
    printf 'package-verify: release rockspec contains local file URL: %s\n' "$artifact" >&2
    exit 1
  fi
  verify_no_local_paths "$artifact" "$artifact"
}

verify_one_source_rock() {
  artifact=$1
  version_value=$(version)
  tmp_dir="$ROOT_DIR/build/package-lua-rock-verify"
  rockspec_name="${PROJECT}-${version_value}-1.rockspec"
  source_name="${PROJECT}-lua-${version_value}.tar.gz"

  rm -rf "$tmp_dir"
  mkdir -p "$tmp_dir"
  unzip -q "$artifact" -d "$tmp_dir"
  test -f "$tmp_dir/$rockspec_name"
  test -f "$tmp_dir/$source_name"
  files=$(find "$tmp_dir" -type f | sed "s#^$tmp_dir/##" | LC_ALL=C sort | tr '\n' ' ')
  if [ "$files" != "$rockspec_name $source_name " ]; then
    printf 'package-verify: source rock payload mismatch in %s\n' "$artifact" >&2
    find "$tmp_dir" -type f | sed "s#^$tmp_dir/##" | LC_ALL=C sort >&2
    exit 1
  fi
  verify_one_rockspec "$tmp_dir/$rockspec_name"
  verify_lua_source_archive "$tmp_dir/$source_name"
  verify_no_local_paths "$artifact" "$tmp_dir"
}

verify_one_archive() {
  artifact=$1
  version_value=$(version)
  tmp_dir="$ROOT_DIR/build/package-verify/$(basename "$artifact" .tar.gz)"
  rm -rf "$tmp_dir"
  mkdir -p "$tmp_dir"
  tar -xzf "$artifact" -C "$tmp_dir"
  roots=$(find "$tmp_dir" -mindepth 1 -maxdepth 1 -type d | wc -l | tr -d ' ')
  if [ "$roots" != "1" ]; then
    printf 'package-verify: %s must contain exactly one root directory\n' "$artifact" >&2
    exit 1
  fi
  root=$(find "$tmp_dir" -mindepth 1 -maxdepth 1 -type d)
  expected=$(basename "$artifact" .tar.gz)
  if [ "$(basename "$root")" != "$expected" ]; then
    printf 'package-verify: %s root mismatch: got %s want %s\n' "$artifact" "$(basename "$root")" "$expected" >&2
    exit 1
  fi

  case "$expected" in
    ${PROJECT}-${version_value})
      verify_no_local_paths "$artifact" "$root"
      verify_source_archive "$artifact"
      return
      ;;
    ${PROJECT}-lua-${version_value})
      verify_no_local_paths "$artifact" "$root"
      verify_lua_source_archive "$artifact"
      return
      ;;
    ${PROJECT}-${version_value}-*)
      target_id=${expected#${PROJECT}-${version_value}-}
      test -f "$root/include/lql/lql.h"
      test -f "$root/include/lql/version.h"
      test -f "$root/lib/liblql.a"
      if [ -f "$root/lib/liblql.so" ]; then
        verify_target_file "$artifact" "$target_id" "$root/lib/liblql.so"
      fi
      if [ -f "$root/lib/liblql.dylib" ]; then
        verify_target_file "$artifact" "$target_id" "$root/lib/liblql.dylib"
      fi
      test -f "$root/lib/cmake/liblql/liblqlConfig.cmake"
      test -f "$root/lib/cmake/liblql/liblqlConfigVersion.cmake"
      test -f "$root/lib/pkgconfig/liblql.pc"
      test -f "$root/share/doc/liblql/LICENSE"
      test -f "$root/share/doc/liblql/README.md"
      verify_dependency_manifest "$artifact" "$root" "$PROJECT" "$target_id" \
        false "external-sdk"
      if [ -e "$root/bin/clql" ]; then
        printf 'package-verify: liblql SDK must not contain clql binary\n' >&2
        exit 1
      fi
      ;;
    ${CLI_PROJECT}-${version_value}-*)
      target_id=${expected#${CLI_PROJECT}-${version_value}-}
      test -x "$root/bin/clql"
      verify_target_file "$artifact" "$target_id" "$root/bin/clql"
      test -d "$root/lib"
      test -f "$root/share/doc/clql/LICENSE"
      test -f "$root/share/doc/clql/README.md"
      verify_dependency_manifest "$artifact" "$root" "$CLI_PROJECT" "$target_id" \
        true "runtime"
      if is_host_smoke_target "${expected#${CLI_PROJECT}-${version_value}-}"; then
        "$root/bin/clql" --version | grep -qx "clql $version_value"
      fi
      if [ -e "$root/include" ] || find "$root/lib" -name 'liblql*' | grep . >/dev/null; then
        printf 'package-verify: clql archive must not contain SDK headers or liblql libraries\n' >&2
        exit 1
      fi
      ;;
    *)
      printf 'package-verify: unexpected artifact name: %s\n' "$expected" >&2
      exit 1
      ;;
  esac

  verify_no_local_paths "$artifact" "$root"
  verify_elf_runtime_paths "$artifact" "$root"
  case "$expected" in
    ${PROJECT}-${version_value}-*) verify_host_consumers "$artifact" "$root" "$target_id" ;;
  esac
}

verify_checksums() {
  manifest=$(manifest_path)
  if [ ! -f "$manifest" ]; then
    package_all
  fi
  if find "$DIST_DIR" -maxdepth 1 -name '*CHECKSUMS' ! -name "$(basename "$manifest")" | grep . >/dev/null; then
    printf 'package-verify: stale checksum manifest found in dist\n' >&2
    exit 1
  fi
  verify_release_artifacts_listed "$manifest"
  (cd "$DIST_DIR" && sha256sum -c "$(basename "$manifest")")
  rm -rf "$ROOT_DIR/build/package-verify"
  while read -r _hash artifact_name; do
    case "$artifact_name" in
      *.tar.gz) verify_one_archive "$DIST_DIR/$artifact_name" ;;
      *.rockspec) verify_one_rockspec "$DIST_DIR/$artifact_name" ;;
      *.src.rock) verify_one_source_rock "$DIST_DIR/$artifact_name" ;;
      *) printf 'package-verify: unsupported checksum artifact: %s\n' "$artifact_name" >&2; exit 1 ;;
    esac
  done <"$manifest"
  verify_no_local_paths "$(basename "$manifest")" "$manifest"
  printf 'package-verify: verified %s\n' "$manifest"
}

verify_release_artifacts_listed() {
  manifest=$1
  listed="$ROOT_DIR/build/package-verify-listed.txt"
  actual="$ROOT_DIR/build/package-verify-actual.txt"

  mkdir -p "$ROOT_DIR/build"
  awk '{print $2}' "$manifest" | LC_ALL=C sort -u >"$listed"
  (
    cd "$DIST_DIR"
    for artifact in "${PROJECT}-"*.tar.gz "${PROJECT}-"*.rockspec \
      "${PROJECT}-"*.src.rock "${CLI_PROJECT}-"*.tar.gz; do
      [ -e "$artifact" ] || continue
      printf '%s\n' "$artifact"
    done
  ) | LC_ALL=C sort -u >"$actual"

  while IFS= read -r artifact; do
    [ -n "$artifact" ] || continue
    if ! grep -Fxq "$artifact" "$listed"; then
      printf 'package-verify: release artifact missing from checksum manifest: %s\n' "$artifact" >&2
      exit 1
    fi
  done <"$actual"
}

expect_privacy_failure() {
  name=$1
  needle=$2
  tmp_dir=$3
  fixture="$tmp_dir/$name"
  output="$tmp_dir/$name.out"

  mkdir -p "$fixture"
  printf '%s\n' "$needle" >"$fixture/leak.txt"
  if (verify_no_local_paths "$name" "$fixture") >"$output" 2>&1; then
    printf 'package privacy fixture unexpectedly passed: %s\n' "$name" >&2
    exit 1
  fi
  if ! grep -F 'package-verify: local path leak' "$output" >/dev/null; then
    printf 'package privacy fixture did not report local path leak: %s\n' "$name" >&2
    cat "$output" >&2
    exit 1
  fi
}

expect_runtime_path_failure() {
  tmp_dir=$1
  cc=${CC:-cc}
  fixture="$tmp_dir/elf-rpath"
  output="$tmp_dir/elf-rpath.out"

  if ! command -v readelf >/dev/null 2>&1; then
    printf 'package privacy fixture: skipping ELF runtime path check; readelf unavailable\n'
    return
  fi
  if ! command -v "$cc" >/dev/null 2>&1; then
    printf 'package privacy fixture: skipping ELF runtime path check; compiler unavailable: %s\n' "$cc"
    return
  fi

  mkdir -p "$fixture"
  cat >"$fixture/smoke.c" <<'EOF'
int main(void) {
  return 0;
}
EOF
  if ! "$cc" "$fixture/smoke.c" -Wl,-rpath,/tmp/liblql-package-rpath-fixture \
    -o "$fixture/smoke" >/dev/null 2>&1; then
    printf 'package privacy fixture: skipping ELF runtime path check; compiler cannot create RUNPATH fixture\n'
    return
  fi
  if (verify_elf_runtime_paths elf-rpath "$fixture") >"$output" 2>&1; then
    printf 'package privacy fixture unexpectedly accepted absolute runtime path\n' >&2
    exit 1
  fi
  if ! grep -F 'package-verify: non-relocatable runtime path' "$output" >/dev/null; then
    printf 'package privacy fixture did not report non-relocatable runtime path\n' >&2
    cat "$output" >&2
    exit 1
  fi
}

check_package_privacy_fixtures() {
  tmp_dir="$ROOT_DIR/build/package-privacy-fixtures"

  rm -rf "$tmp_dir"
  mkdir -p "$tmp_dir"
  expect_privacy_failure repo-path "$ROOT_DIR" "$tmp_dir"
  expect_privacy_failure home-path "$HOME" "$tmp_dir"
  expect_privacy_failure repo-file-url "file://$ROOT_DIR" "$tmp_dir"
  expect_privacy_failure home-file-url "file://$HOME" "$tmp_dir"
  expect_runtime_path_failure "$tmp_dir"
}

check_package_manifest_fixtures() {
  tmp_dir="$ROOT_DIR/build/package-manifest-fixtures"
  old_dist=$DIST_DIR
  old_root=$ROOT_DIR

  rm -rf "$tmp_dir"
  mkdir -p "$tmp_dir/dist" "$tmp_dir/build"
  DIST_DIR="$tmp_dir/dist"
  ROOT_DIR="$tmp_dir"
  printf 'listed\n' >"$DIST_DIR/${PROJECT}-0.0.0.tar.gz"
  printf 'unlisted\n' >"$DIST_DIR/${CLI_PROJECT}-0.0.0-x86_64-linux-gnu.tar.gz"
  (
    cd "$DIST_DIR"
    sha256sum "${PROJECT}-0.0.0.tar.gz" >"${PROJECT}-0.0.0-CHECKSUMS"
  )
  if (verify_release_artifacts_listed "$DIST_DIR/${PROJECT}-0.0.0-CHECKSUMS") \
    >"$tmp_dir/out" 2>&1; then
    printf 'package manifest fixture unexpectedly accepted unlisted artifact\n' >&2
    ROOT_DIR=$old_root
    DIST_DIR=$old_dist
    exit 1
  fi
  if ! grep -F 'release artifact missing from checksum manifest' "$tmp_dir/out" >/dev/null; then
    printf 'package manifest fixture did not report unlisted artifact\n' >&2
    cat "$tmp_dir/out" >&2
    ROOT_DIR=$old_root
    DIST_DIR=$old_dist
    exit 1
  fi
  ROOT_DIR=$old_root
  DIST_DIR=$old_dist
}

check_lua_package_contract_fixtures() {
  tmp_dir="$ROOT_DIR/build/package-lua-contract-fixtures"
  version_value=$(version)
  bad_rockspec="$tmp_dir/${PROJECT}-${version_value}-1.rockspec"
  source_root="$tmp_dir/${PROJECT}-lua-${version_value}"
  bad_source="$tmp_dir/${PROJECT}-lua-${version_value}.tar.gz"

  rm -rf "$tmp_dir"
  mkdir -p "$tmp_dir" "$source_root/lua" "$source_root/lua/tests" \
    "$source_root/lua/benchmarks" "$source_root/scripts"

  cat >"$bad_rockspec" <<EOF
package = "liblql"
version = "${version_value}-1"

source = {
   url = "https://github.com/sa6mwa/liblql/releases/download/v${version_value}/${PROJECT}-lua-${version_value}.tar.gz",
   dir = "${PROJECT}-lua-${version_value}"
}

dependencies = {
   "lua >= 5.4"
}
EOF
  if (verify_one_rockspec "$bad_rockspec") >/dev/null 2>&1; then
    printf 'package Lua contract fixture unexpectedly accepted broad Lua dependency\n' >&2
    exit 1
  fi

  printf '%s\n' "$version_value" >"$source_root/VERSION"
  printf '%s\n' 'MIT' >"$source_root/LICENSE"
  printf '%s\n' '# fixture' >"$source_root/README.md"
  printf '%s\n' 'return require("lql.core")' >"$source_root/lua/lql.lua"
  cat >"$source_root/lua/lql_core.c" <<'EOF'
#include <lua.h>
int luaopen_lql_core(lua_State *L) { (void)L; return 0; }
EOF
  printf '%s\n' '-- fixture' >"$source_root/lua/tests/lql_smoke.lua"
  printf '%s\n' '-- fixture' >"$source_root/lua/tests/lql_core_smoke.lua"
  printf '%s\n' '-- fixture' >"$source_root/lua/benchmarks/parity.lua"
  cp "$ROOT_DIR/liblql.rockspec.in" "$source_root/liblql.rockspec.in"
  cp "$ROOT_DIR/scripts/build_lua_rock.sh" "$source_root/scripts/build_lua_rock.sh"
  cp "$ROOT_DIR/scripts/check_lua_runtime_fixtures.sh" "$source_root/scripts/check_lua_runtime_fixtures.sh"
  cp "$ROOT_DIR/scripts/release_version.sh" "$source_root/scripts/release_version.sh"
  cp "$ROOT_DIR/scripts/render_release_rockspec.sh" "$source_root/scripts/render_release_rockspec.sh"
  cp "$ROOT_DIR/scripts/run_lua_tests.sh" "$source_root/scripts/run_lua_tests.sh"
  cp "$ROOT_DIR/scripts/stage_lua_rock_sources.sh" "$source_root/scripts/stage_lua_rock_sources.sh"
  (cd "$source_root" && find . -type f | sed 's#^\./##' | LC_ALL=C sort) \
    >"$source_root/RELEASE_MANIFEST"
  (cd "$tmp_dir" && tar -czf "$bad_source" "${PROJECT}-lua-${version_value}")
  if (verify_lua_source_archive "$bad_source") >/dev/null 2>&1; then
    printf 'package Lua contract fixture unexpectedly accepted missing Lua 5.5 source guard\n' >&2
    exit 1
  fi
}

check_source_manifest_fixtures() {
  tmp_dir="$ROOT_DIR/build/package-source-manifest-fixtures"
  manifest="$tmp_dir/manifest"
  expected="$tmp_dir/expected"
  bad="$tmp_dir/bad"

  if [ ! -d "$ROOT_DIR/.git" ]; then
    printf 'package source manifest fixture: skipping current-tree check outside git worktree\n'
    return
  fi
  rm -rf "$tmp_dir"
  mkdir -p "$tmp_dir"
  write_current_source_manifest "$manifest"
  if ! verify_source_manifest_matches_current "$manifest" fixture "$expected"; then
    printf 'package source manifest fixture unexpectedly rejected current manifest\n' >&2
    exit 1
  fi
  sed '1d' "$manifest" >"$bad"
  if (verify_source_manifest_matches_current "$bad" fixture "$expected") \
    >/dev/null 2>&1; then
    printf 'package source manifest fixture unexpectedly accepted stale manifest\n' >&2
    exit 1
  fi
}

case "$TARGET" in
  package)
    package_all
    ;;
  package-source)
    mkdir -p "$DIST_DIR"
    package_source
    ;;
  package-source-smoke)
    mkdir -p "$DIST_DIR"
    package_source
    verify_one_archive "$DIST_DIR/${PROJECT}-$(version).tar.gz"
    ;;
  package-checksums)
    write_checksums
    ;;
  release-lua-artifacts)
    mkdir -p "$DIST_DIR"
    package_lua_source
    package_lua_rockspec
    package_lua_source_rock
    write_checksums
    ;;
  package-verify|verify-release-archives|verify-release-privacy)
    verify_checksums
    ;;
  package-privacy-fixtures)
    check_package_privacy_fixtures
    ;;
  package-manifest-fixtures)
    check_package_manifest_fixtures
    ;;
  package-lua-contract-fixtures)
    check_lua_package_contract_fixtures
    ;;
  package-source-manifest-fixtures)
    check_source_manifest_fixtures
    ;;
  release-matrix)
    MATRIX_MODE=1
    TARGETS=${LQL_PACKAGE_TARGETS:-$MATRIX_TARGETS}
    package_all
    verify_checksums
    ;;
  print-manifest)
    manifest_path
    ;;
  *)
    printf 'unknown package target: %s\n' "$TARGET" >&2
    exit 2
    ;;
esac
