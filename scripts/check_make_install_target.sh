#!/bin/sh
set -eu

check_install_surface() {
  makefile=$1
  build_script=$2
  cmakelists=$3

  if ! grep -F 'make build                   build static clql' "$makefile" >/dev/null; then
    printf 'install surface: make help must advertise static clql build\n' >&2
    return 1
  fi
  if ! grep -F 'make install                 install built clql' "$makefile" >/dev/null; then
    printf 'install surface: make help must advertise make install\n' >&2
    return 1
  fi
  if ! grep -Eq '^build: build-clql-static' "$makefile"; then
    printf 'install surface: make build must delegate to build-clql-static\n' >&2
    return 1
  fi
  if ! grep -F './scripts/build_clql_static.sh' "$makefile" >/dev/null; then
    printf 'install surface: build-clql-static must call scripts/build_clql_static.sh\n' >&2
    return 1
  fi
  if ! grep -Eq '^install: build' "$makefile"; then
    printf 'install surface: make install must depend on build\n' >&2
    return 1
  fi
  if ! grep -F '$(DESTDIR)$(BINDIR)/clql' "$makefile" >/dev/null; then
    printf 'install surface: make install must honor DESTDIR and BINDIR\n' >&2
    return 1
  fi
  if ! grep -F 'PREFIX ?= /usr/local' "$makefile" >/dev/null ||
     ! grep -F 'BINDIR ?= $(PREFIX)/bin' "$makefile" >/dev/null; then
    printf 'install surface: default install path must be /usr/local/bin\n' >&2
    return 1
  fi

  if ! grep -F 'uname -s' "$build_script" >/dev/null ||
     ! grep -F 'uname -m' "$build_script" >/dev/null; then
    printf 'install surface: static clql build must select the host OS/arch\n' >&2
    return 1
  fi
  if ! grep -F 'musl_target="${arch}-linux-musl"' "$build_script" >/dev/null ||
     ! grep -F 'gnu_target="${arch}-linux-gnu"' "$build_script" >/dev/null; then
    printf 'install surface: Linux build must prefer host musl then GNU\n' >&2
    return 1
  fi
  if ! grep -F 'compiler_can_link "$musl_cc"' "$build_script" >/dev/null ||
     ! grep -F 'compiler_can_link "$gnu_cc"' "$build_script" >/dev/null; then
    printf 'install surface: static clql build must verify static linkability\n' >&2
    return 1
  fi
  if ! grep -F -- '-DLQL_LONEJSON_STATIC=ON' "$build_script" >/dev/null ||
     ! grep -F -- '-DLQL_CLQL_STATIC_LINK=ON' "$build_script" >/dev/null; then
    printf 'install surface: static clql build must request static link mode\n' >&2
    return 1
  fi
  if ! grep -F 'option(LQL_CLQL_STATIC_LINK' "$cmakelists" >/dev/null ||
     ! grep -F 'target_link_options(clql PRIVATE -static)' "$cmakelists" >/dev/null; then
    printf 'install surface: CMake must expose static clql link mode\n' >&2
    return 1
  fi
}

if [ "${1:-}" = "--fixtures" ]; then
  tmp=${TMPDIR:-/tmp}/liblql-install-surface-fixtures.$$
  trap 'rm -rf "$tmp"' EXIT HUP INT TERM
  mkdir -p "$tmp/scripts"
  makefile="$tmp/Makefile"
  build_script="$tmp/scripts/build_clql_static.sh"
  cmakelists="$tmp/CMakeLists.txt"

  cat >"$makefile" <<'EOF'
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
help:
	@printf '%s\n' 'make build                   build static clql, preferring musl then GNU' 'make install                 install built clql to $${PREFIX:-/usr/local}/bin'
build: build-clql-static
build-clql-static:
	@./scripts/build_clql_static.sh
install: build
	@install -m 0755 build/clql-static/clql "$(DESTDIR)$(BINDIR)/clql"
EOF
  cat >"$build_script" <<'EOF'
os=$(uname -s)
machine=$(uname -m)
musl_target="${arch}-linux-musl"
gnu_target="${arch}-linux-gnu"
compiler_can_link "$musl_cc"
compiler_can_link "$gnu_cc"
cmake -DLQL_LONEJSON_STATIC=ON -DLQL_CLQL_STATIC_LINK=ON
EOF
  cat >"$cmakelists" <<'EOF'
option(LQL_CLQL_STATIC_LINK "Link clql as a static executable where supported" OFF)
target_link_options(clql PRIVATE -static)
EOF
  check_install_surface "$makefile" "$build_script" "$cmakelists"

  sed '/DESTDIR/d' "$makefile" >"$tmp/no-destdir.mk"
  if check_install_surface "$tmp/no-destdir.mk" "$build_script" \
    "$cmakelists" >/dev/null 2>&1; then
    printf 'install surface fixture: expected missing DESTDIR to fail\n' >&2
    exit 1
  fi

  sed '/LQL_LONEJSON_STATIC/d' "$build_script" >"$tmp/no-static-dep.sh"
  if check_install_surface "$makefile" "$tmp/no-static-dep.sh" \
    "$cmakelists" >/dev/null 2>&1; then
    printf 'install surface fixture: expected missing static dependency mode to fail\n' >&2
    exit 1
  fi
  exit 0
fi

makefile=${1:?usage: check_make_install_target.sh MAKEFILE BUILD_SCRIPT CMAKELISTS}
build_script=${2:?usage: check_make_install_target.sh MAKEFILE BUILD_SCRIPT CMAKELISTS}
cmakelists=${3:?usage: check_make_install_target.sh MAKEFILE BUILD_SCRIPT CMAKELISTS}

check_install_surface "$makefile" "$build_script" "$cmakelists"
