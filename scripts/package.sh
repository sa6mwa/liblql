#!/bin/sh
set -eu

PROJECT=liblql
CLI_PROJECT=clql
TARGET=${1:-package}
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DIST_DIR=${LQL_DIST_DIR:-"$ROOT_DIR/dist"}
TARGETS=${LQL_PACKAGE_TARGETS:-x86_64-linux-gnu}
MATRIX_TARGETS="x86_64-linux-gnu x86_64-linux-musl aarch64-linux-gnu aarch64-linux-musl armhf-linux-gnu armhf-linux-musl arm64-apple-darwin"

version() {
  (cd "$ROOT_DIR" && ./scripts/release_version.sh)
}

manifest_path() {
  printf '%s/%s-%s-CHECKSUMS\n' "$DIST_DIR" "$PROJECT" "$(version)"
}

clean_dist() {
  mkdir -p "$DIST_DIR"
  find "$DIST_DIR" -maxdepth 1 \( \
    -name "${PROJECT}-*.tar.gz" -o \
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

  (cd "$ROOT_DIR" && cmake --preset "$preset")
  (cd "$ROOT_DIR" && cmake --build --preset "$preset")
  rm -rf "$work_dir"
  mkdir -p "$install_root"
  cmake --install "$build_dir" --prefix "$install_root" --strip

  cp -R "$install_root" "$lib_root"
  rm -rf "$lib_root/bin"

  mkdir -p "$cli_root/bin" "$cli_root/lib" "$cli_root/share/doc/$CLI_PROJECT" \
    "$cli_root/share/doc/lonejson"
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

  make_tar_gz "$lib_root" "$DIST_DIR/${PROJECT}-${version_value}-${target_id}.tar.gz"
  make_tar_gz "$cli_root" "$DIST_DIR/${CLI_PROJECT}-${version_value}-${target_id}.tar.gz"
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

package_all() {
  clean_dist
  for target_id in $TARGETS; do
    package_one "$target_id"
  done
  package_source
  write_checksums
}

write_checksums() {
  version_value=$(version)
  manifest=$(manifest_path)
  tmp="${manifest}.tmp"
  rm -f "$tmp"
  (
    cd "$DIST_DIR"
    for artifact in "${PROJECT}-${version_value}.tar.gz" "${PROJECT}-${version_value}"-*.tar.gz "${CLI_PROJECT}-${version_value}"-*.tar.gz; do
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
  lql_selector *selector = NULL;
  lql_error error;
  lql_status status;
  lql_error_init(&error);
  status = lql_selector_parse(expr, &selector, &error);
  if (status != LQL_STATUS_OK) {
    fprintf(stderr, "parse failed: %s\n", error.message);
    return 1;
  }
  if (strcmp(lql_version(), LQL_VERSION) != 0) {
    fprintf(stderr, "version mismatch: %s != %s\n", lql_version(), LQL_VERSION);
    return 1;
  }
  {
    lql_capabilities caps;
    lql_capabilities_get(&caps);
    if (!caps.selector_parse || !caps.mutation_file_range ||
        !caps.mutation_buffered_json) {
      fprintf(stderr, "capability query mismatch\n");
      return 1;
    }
  }
  lql_selector_free(selector);
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
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()
find_package(liblql CONFIG REQUIRED)
add_executable(consumer ../consumer.c)
target_link_libraries(consumer PRIVATE liblql::lql_static)
EOF
  cmake -S "$smoke_dir/cmake" -B "$smoke_dir/cmake-build" \
    -DCMAKE_PREFIX_PATH="$root;$dep_root" >/dev/null
  cmake --build "$smoke_dir/cmake-build" >/dev/null
  "$smoke_dir/cmake-build/consumer"

  if command -v pkg-config >/dev/null 2>&1; then
    PKG_CONFIG_PATH="$root/lib/pkgconfig:$dep_root/lib/pkgconfig" \
      "$cc" -std=c90 -Wall -Wextra -Wpedantic -Werror \
      "$smoke_dir/consumer.c" \
      $(PKG_CONFIG_PATH="$root/lib/pkgconfig:$dep_root/lib/pkgconfig" pkg-config --cflags --libs --static liblql) \
      -o "$smoke_dir/pkgconfig/consumer"
    LD_LIBRARY_PATH="$root/lib:$dep_root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
      "$smoke_dir/pkgconfig/consumer"
  else
    printf 'package-verify: pkg-config unavailable; skipping pkg-config consumer smoke for %s\n' "$artifact"
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
    ${PROJECT}-${version_value}-*)
      target_id=${expected#${PROJECT}-${version_value}-}
      test -f "$root/include/lql/lql.h"
      test -f "$root/include/lql/version.h"
      test -f "$root/lib/liblql.a"
      test -f "$root/lib/cmake/liblql/liblqlConfig.cmake"
      test -f "$root/lib/cmake/liblql/liblqlConfigVersion.cmake"
      test -f "$root/lib/pkgconfig/liblql.pc"
      test -f "$root/share/doc/liblql/LICENSE"
      test -f "$root/share/doc/liblql/README.md"
      if [ -e "$root/bin/clql" ]; then
        printf 'package-verify: liblql SDK must not contain clql binary\n' >&2
        exit 1
      fi
      ;;
    ${CLI_PROJECT}-${version_value}-*)
      test -x "$root/bin/clql"
      test -d "$root/lib"
      test -f "$root/share/doc/clql/LICENSE"
      test -f "$root/share/doc/clql/README.md"
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
  (cd "$DIST_DIR" && sha256sum -c "$(basename "$manifest")")
  rm -rf "$ROOT_DIR/build/package-verify"
  while read -r _hash artifact_name; do
    case "$artifact_name" in
      *.tar.gz) verify_one_archive "$DIST_DIR/$artifact_name" ;;
      *) printf 'package-verify: unsupported checksum artifact: %s\n' "$artifact_name" >&2; exit 1 ;;
    esac
  done <"$manifest"
  verify_no_local_paths "$(basename "$manifest")" "$manifest"
  printf 'package-verify: verified %s\n' "$manifest"
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
  package-verify|verify-release-archives|verify-release-privacy)
    verify_checksums
    ;;
  release-matrix)
    TARGETS=${LQL_PACKAGE_TARGETS:-$MATRIX_TARGETS}
    package_all
    verify_checksums
    ;;
  *)
    printf 'unknown package target: %s\n' "$TARGET" >&2
    exit 2
    ;;
esac
