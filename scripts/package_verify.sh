#!/bin/sh
set -eu

project=liblql
dist_dir=${LQL_DIST_DIR:-dist}
target_arg=${1:-all}
work=${LQL_PACKAGE_VERIFY_DIR:-build/package-verify}

find_manifest() {
  manifests=$(find "$dist_dir" -maxdepth 1 -name "$project-*-CHECKSUMS" -type f | sort)
  count=$(printf '%s\n' "$manifests" | sed '/^$/d' | wc -l | tr -d ' ')
  if [ "$count" != 1 ]; then
    printf 'package verify: expected exactly one checksum manifest, found %s\n' "$count" >&2
    printf '%s\n' "$manifests" >&2
    exit 1
  fi
  printf '%s\n' "$manifests"
}

manifest=$(find_manifest)
version=$(basename "$manifest" | sed "s/^$project-//;s/-CHECKSUMS\$//")

(
  cd "$dist_dir"
  sha256sum -c "$(basename "$manifest")" >/dev/null
)

listed=$(awk '{print $2}' "$manifest" | sort)
release_artifacts=$(find "$dist_dir" -maxdepth 1 -type f \
  \( -name "$project-*.tar.gz" -o -name "$project-*-1.rockspec" -o \
     -name "$project-*-1.src.rock" \) -printf '%f\n' | sort)
if [ "$listed" != "$release_artifacts" ]; then
  printf 'package verify: checksum manifest does not match release artifacts\n' >&2
  printf 'listed:\n%s\nartifacts:\n%s\n' "$listed" "$release_artifacts" >&2
  exit 1
fi

rm -rf "$work"
mkdir -p "$work/extract"

verify_privacy() {
  archive=$1
  prefix=$2
  repo=$(pwd -P)
  home=${HOME:-}
  if grep -R -a -n -F "$repo" "$prefix" >/dev/null 2>&1; then
    printf 'package verify: repository path leaked into %s\n' "$archive" >&2
    grep -R -a -n -F "$repo" "$prefix" >&2
    exit 1
  fi
  if [ -n "$home" ] && grep -R -a -n -F "$home" "$prefix" >/dev/null 2>&1; then
    printf 'package verify: HOME path leaked into %s\n' "$archive" >&2
    grep -R -a -n -F "$home" "$prefix" >&2
    exit 1
  fi
  if grep -R -a -n -E 'file://|/tmp/|/var/tmp/' "$prefix" >/dev/null 2>&1; then
    printf 'package verify: local URL or temp path leaked into %s\n' "$archive" >&2
    grep -R -a -n -E 'file://|/tmp/|/var/tmp/' "$prefix" >&2
    exit 1
  fi
}

verify_source_privacy() {
  archive=$1
  prefix=$2
  repo=$(pwd -P)
  home=${HOME:-}
  if grep -R -a -n -F "$repo" "$prefix" >/dev/null 2>&1; then
    printf 'package verify: repository path leaked into %s\n' "$archive" >&2
    grep -R -a -n -F "$repo" "$prefix" >&2
    exit 1
  fi
  if [ -n "$home" ] && grep -R -a -n -F "$home" "$prefix" >/dev/null 2>&1; then
    printf 'package verify: HOME path leaked into %s\n' "$archive" >&2
    grep -R -a -n -F "$home" "$prefix" >&2
    exit 1
  fi
  if grep -R -a -n -F "file://$repo" "$prefix" >/dev/null 2>&1; then
    printf 'package verify: repository file URL leaked into %s\n' "$archive" >&2
    grep -R -a -n -F "file://$repo" "$prefix" >&2
    exit 1
  fi
  if [ -n "$home" ] && grep -R -a -n -F "file://$home" "$prefix" >/dev/null 2>&1; then
    printf 'package verify: HOME file URL leaked into %s\n' "$archive" >&2
    grep -R -a -n -F "file://$home" "$prefix" >&2
    exit 1
  fi
}

verify_linux_runtime_paths() {
  target=$1
  prefix=$2
  tools=$work/tools-$target.env
  scripts/discover_target_tools.sh "build/$target-release" "$target" >"$tools"
  readelf_tool=$(sed -n 's/^READELF=//p' "$tools")
  if [ -z "$readelf_tool" ] || [ ! -x "$readelf_tool" ]; then
    printf 'package verify: missing readelf for %s\n' "$target" >&2
    exit 1
  fi
  for elf in "$prefix/lib/liblql.so.0" "$prefix/bin/clql"; do
    if "$readelf_tool" -d "$elf" | grep -E 'RPATH|RUNPATH' >/dev/null 2>&1; then
      if "$readelf_tool" -d "$elf" | grep -E 'RPATH|RUNPATH' |
        grep -v '\$ORIGIN' >/dev/null 2>&1; then
        printf 'package verify: non-relocatable runtime path in %s\n' "$elf" >&2
        "$readelf_tool" -d "$elf" | grep -E 'RPATH|RUNPATH' >&2
        exit 1
      fi
    fi
  done
}

verify_darwin_runtime_paths() {
  target=$1
  prefix=$2
  tools=$work/tools-$target.env
  scripts/discover_target_tools.sh "build/$target-release" "$target" >"$tools"
  otool=$(sed -n 's/^OTOOL=//p' "$tools")
  if [ -z "$otool" ] || [ ! -x "$otool" ]; then
    printf 'package verify: missing otool for %s\n' "$target" >&2
    exit 1
  fi
  for macho in "$prefix/lib/liblql.0.dylib" "$prefix/bin/clql"; do
    if "$otool" -L "$macho" |
      grep -E "$HOME|$(pwd -P)|/tmp/|/var/tmp/|/usr/local/" >/dev/null 2>&1; then
      printf 'package verify: non-relocatable Darwin dependency in %s\n' "$macho" >&2
      "$otool" -L "$macho" >&2
      exit 1
    fi
  done
  if ! "$otool" -D "$prefix/lib/liblql.0.dylib" |
    grep '@rpath/liblql.0.dylib' >/dev/null 2>&1; then
    printf 'package verify: Darwin install name is not @rpath/liblql.0.dylib\n' >&2
    "$otool" -D "$prefix/lib/liblql.0.dylib" >&2
    exit 1
  fi
}

verify_payload() {
  target=$1
  archive=$dist_dir/$project-$version-$target.tar.gz
  root=$project-$version-$target
  extract_dir=$work/extract/$target
  prefix=$extract_dir/$root

  if [ ! -f "$archive" ]; then
    printf 'package verify: missing target archive: %s\n' "$archive" >&2
    exit 1
  fi
  mkdir -p "$extract_dir"
  tar -xzf "$archive" -C "$extract_dir"
  roots=$(find "$extract_dir" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | sort)
  if [ "$roots" != "$root" ]; then
    printf 'package verify: archive root mismatch: expected %s got %s\n' \
      "$root" "$roots" >&2
    exit 1
  fi

  for path in \
    include/lql/lql.h \
    include/lql/version.h \
    lib/liblql.a \
    lib/cmake/liblql/liblqlConfig.cmake \
    lib/pkgconfig/liblql.pc \
    bin/clql \
    share/doc/liblql/LICENSE \
    share/doc/liblql/README.md
  do
    if [ ! -e "$prefix/$path" ]; then
      printf 'package verify: missing payload path: %s in %s\n' "$path" "$target" >&2
      exit 1
    fi
  done

  case "$target" in
    *-darwin)
      for path in lib/liblql.dylib lib/liblql.0.dylib; do
        if [ ! -e "$prefix/$path" ]; then
          printf 'package verify: missing Darwin payload path: %s\n' "$path" >&2
          exit 1
        fi
      done
      verify_darwin_runtime_paths "$target" "$prefix"
      ;;
    *-linux-*)
      for path in lib/liblql.so lib/liblql.so.0; do
        if [ ! -e "$prefix/$path" ]; then
          printf 'package verify: missing Linux payload path: %s\n' "$path" >&2
          exit 1
        fi
      done
      verify_linux_runtime_paths "$target" "$prefix"
      ;;
    *)
      printf 'package verify: unsupported target: %s\n' "$target" >&2
      exit 1
      ;;
  esac

  verify_privacy "$archive" "$prefix"

  if [ "$target" = "x86_64-linux-gnu" ] && [ "$(uname -m)" = "x86_64" ]; then
    LQL_INSTALL_PREFIX="$prefix" \
    LQL_INSTALL_CONSUMER_DIR="$work/install-consumer-$target" \
      sh scripts/check_install_tree.sh
  else
    printf 'package verify: skipped executable consumer smoke for cross target %s\n' "$target"
  fi

  printf 'package verify: verified %s\n' "$archive"
}

verify_source_archive() {
  archive=$dist_dir/$project-$version.tar.gz
  root=$project-$version
  extract_dir=$work/extract/source
  prefix=$extract_dir/$root
  source_build=$work/source-build

  if [ ! -f "$archive" ]; then
    printf 'package verify: missing source archive: %s\n' "$archive" >&2
    exit 1
  fi
  mkdir -p "$extract_dir"
  tar -xzf "$archive" -C "$extract_dir"
  roots=$(find "$extract_dir" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | sort)
  if [ "$roots" != "$root" ]; then
    printf 'package verify: source archive root mismatch: expected %s got %s\n' \
      "$root" "$roots" >&2
    exit 1
  fi
  for path in \
    CMakeLists.txt \
    CMakePresets.json \
    Makefile \
    LICENSE \
    README.md \
    VERSION \
    RELEASE_MANIFEST \
    include/lql/lql.h \
    cmake/LqlVersion.cmake \
    scripts/cpkt-toolchains.sh \
    scripts/package-verify.sh \
    scripts/package_lua.sh \
    scripts/package_source.sh \
    scripts/render_release_rockspec.sh \
    scripts/stage_lua_rock_sources.sh \
    lua/bin/lql.lua \
    lua/lql/cli.lua \
    lua/lql_core.c \
    lua/lql/init.lua
  do
    if [ ! -e "$prefix/$path" ]; then
      printf 'package verify: missing source payload path: %s\n' "$path" >&2
      exit 1
    fi
  done
  if find "$prefix" \( -path '*/.git/*' -o -path '*/build/*' -o \
      -path '*/dist/*' -o -path '*/vendor/lonejson/*' \) |
    sed -n '1p' | grep . >/dev/null; then
    printf 'package verify: source archive contains generated or VCS state\n' >&2
    find "$prefix" \( -path '*/.git/*' -o -path '*/build/*' -o \
      -path '*/dist/*' -o -path '*/vendor/lonejson/*' \) >&2
    exit 1
  fi
  source_version=$(sed -n '1p' "$prefix/VERSION")
  if [ "$source_version" != "$version" ]; then
    printf 'package verify: source VERSION mismatch: got %s want %s\n' \
      "$source_version" "$version" >&2
    exit 1
  fi
  if ! grep '^VERSION$' "$prefix/RELEASE_MANIFEST" >/dev/null ||
    ! grep '^RELEASE_MANIFEST$' "$prefix/RELEASE_MANIFEST" >/dev/null; then
    printf 'package verify: source RELEASE_MANIFEST omits injected files\n' >&2
    exit 1
  fi
  verify_source_privacy "$archive" "$prefix"
  prefix_abs=$(cd "$prefix" && pwd -P)
  source_build_abs=$(mkdir -p "$source_build" && cd "$source_build" && pwd -P)
  cmake -S "$prefix_abs" -B "$source_build_abs" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$prefix_abs/cmake/cpkt-toolchain.cmake" \
    -DLQL_TARGET_ID=x86_64-linux-gnu >/dev/null
  cmake --build "$source_build_abs" >/dev/null
  ctest --test-dir "$source_build_abs" --output-on-failure >/dev/null
  if ! grep '#define LQL_VERSION "'$version'"' \
    "$source_build_abs/generated/include/lql/version.h" >/dev/null; then
    printf 'package verify: source build generated wrong version\n' >&2
    exit 1
  fi
  printf 'package verify: verified %s\n' "$archive"
}

verify_lua_source_archive() {
  archive=$dist_dir/$project-lua-$version.tar.gz
  root=$project-lua-$version
  extract_dir=$work/extract/lua-source
  prefix=$extract_dir/$root

  if [ ! -f "$archive" ]; then
    printf 'package verify: missing Lua source archive: %s\n' "$archive" >&2
    exit 1
  fi
  mkdir -p "$extract_dir"
  tar -xzf "$archive" -C "$extract_dir"
  roots=$(find "$extract_dir" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | sort)
  if [ "$roots" != "$root" ]; then
    printf 'package verify: Lua source archive root mismatch: expected %s got %s\n' \
      "$root" "$roots" >&2
    exit 1
  fi
  for path in \
    LICENSE \
    README.md \
    VERSION \
    RELEASE_MANIFEST \
    include/lql/lql.h \
    include/lql/version.h \
    liblql-dev-1.rockspec.in \
    lua/bin/lql.lua \
    lua/lql/cli.lua \
    lua/lql_core.c \
    lua/lql/init.lua \
    scripts/build_lua_rock.sh \
    scripts/render_release_rockspec.sh \
    scripts/run_lua_tests.sh \
    scripts/stage_lua_rock_sources.sh \
    "$project-$version-1.rockspec"
  do
    if [ ! -e "$prefix/$path" ]; then
      printf 'package verify: missing Lua source payload path: %s\n' "$path" >&2
      exit 1
    fi
  done
  if find "$prefix" \( -name '*.so' -o -name '*.dylib' -o -name '*.a' -o \
      -path '*/build/*' -o -path '*/dist/*' -o -path '*/.git/*' \) |
    sed -n '1p' | grep . >/dev/null; then
    printf 'package verify: Lua source archive contains binary/generated/VCS state\n' >&2
    find "$prefix" \( -name '*.so' -o -name '*.dylib' -o -name '*.a' -o \
      -path '*/build/*' -o -path '*/dist/*' -o -path '*/.git/*' \) >&2
    exit 1
  fi
  lua_version=$(sed -n '1p' "$prefix/VERSION")
  if [ "$lua_version" != "$version" ]; then
    printf 'package verify: Lua source VERSION mismatch: got %s want %s\n' \
      "$lua_version" "$version" >&2
    exit 1
  fi
  verify_source_privacy "$archive" "$prefix"
  printf 'package verify: verified %s\n' "$archive"
}

verify_lua_rockspec() {
  rockspec=$dist_dir/$project-$version-1.rockspec
  if [ ! -f "$rockspec" ]; then
    printf 'package verify: missing Lua rockspec: %s\n' "$rockspec" >&2
    exit 1
  fi
  if grep -a -n -F "$(pwd -P)" "$rockspec" >/dev/null ||
    { [ -n "${HOME:-}" ] && grep -a -n -F "$HOME" "$rockspec" >/dev/null; } ||
    grep -a -n -E 'file://|/tmp/|/var/tmp/' "$rockspec" >/dev/null; then
    printf 'package verify: local path leaked into Lua rockspec: %s\n' "$rockspec" >&2
    exit 1
  fi
  if ! grep "version = \"$version-1\"" "$rockspec" >/dev/null ||
    ! grep "url = \"$project-lua-$version.tar.gz\"" "$rockspec" >/dev/null ||
    ! grep "dir = \"$project-lua-$version\"" "$rockspec" >/dev/null; then
    printf 'package verify: Lua rockspec version/source fields are wrong\n' >&2
    exit 1
  fi
  luarocks --lua-version=5.5 lint "$rockspec" >/dev/null
  printf 'package verify: verified %s\n' "$rockspec"
}

verify_lua_src_rock() {
  rock=$dist_dir/$project-$version-1.src.rock
  extract_dir=$work/extract/lua-src-rock
  if [ ! -f "$rock" ]; then
    printf 'package verify: missing Lua source rock: %s\n' "$rock" >&2
    exit 1
  fi
  rm -rf "$extract_dir"
  mkdir -p "$extract_dir"
  unzip -q "$rock" -d "$extract_dir"
  for path in "$project-$version-1.rockspec" "$project-lua-$version.tar.gz"; do
    if [ ! -f "$extract_dir/$path" ]; then
      printf 'package verify: source rock missing payload: %s\n' "$path" >&2
      exit 1
    fi
  done
  cmp -s "$dist_dir/$project-$version-1.rockspec" \
    "$extract_dir/$project-$version-1.rockspec" || {
      printf 'package verify: source rock rockspec differs from dist rockspec\n' >&2
      exit 1
    }
  cmp -s "$dist_dir/$project-lua-$version.tar.gz" \
    "$extract_dir/$project-lua-$version.tar.gz" || {
      printf 'package verify: source rock Lua source archive differs from dist archive\n' >&2
      exit 1
    }
  nested="$extract_dir/nested"
  mkdir -p "$nested"
  tar -xzf "$extract_dir/$project-lua-$version.tar.gz" -C "$nested"
  if [ ! -f "$nested/$project-lua-$version/lua/bin/lql.lua" ] ||
    [ ! -f "$nested/$project-lua-$version/lua/lql/cli.lua" ]; then
    printf 'package verify: source rock nested Lua source package omits lql.lua CLI\n' >&2
    exit 1
  fi
  if grep -R -a -n -F "$(pwd -P)" "$extract_dir" >/dev/null ||
    { [ -n "${HOME:-}" ] && grep -R -a -n -F "$HOME" "$extract_dir" >/dev/null; } ||
    grep -R -a -n -E 'file://|/tmp/|/var/tmp/' "$extract_dir" >/dev/null; then
    printf 'package verify: local path leaked into Lua source rock: %s\n' "$rock" >&2
    exit 1
  fi
  printf 'package verify: verified %s\n' "$rock"
}

if [ "$target_arg" = "all" ]; then
  awk '{print $2}' "$manifest" | sort | while IFS= read -r artifact; do
    case "$artifact" in
      "$project-$version.tar.gz")
        verify_source_archive
        ;;
      "$project-lua-$version.tar.gz")
        verify_lua_source_archive
        ;;
      "$project-$version-1.rockspec")
        verify_lua_rockspec
        ;;
      "$project-$version-1.src.rock")
        verify_lua_src_rock
        ;;
      "$project-$version-"*.tar.gz)
        target=$(printf '%s\n' "$artifact" |
          sed "s/^$project-$version-//;s/\\.tar\\.gz\$//")
        verify_payload "$target"
        ;;
      *)
        printf 'package verify: unsupported release artifact: %s\n' "$artifact" >&2
        exit 1
        ;;
    esac
  done
else
  if [ "$target_arg" = "checksums" ]; then
    printf 'package verify: verified checksum manifest %s\n' "$manifest"
  elif [ "$target_arg" = "archives" ] || [ "$target_arg" = "privacy" ]; then
    "$0" all
  elif [ "$target_arg" = "source" ]; then
    verify_source_archive
  elif [ "$target_arg" = "lua" ]; then
    verify_lua_source_archive
    verify_lua_rockspec
    verify_lua_src_rock
  else
    verify_payload "$target_arg"
  fi
fi
