#!/usr/bin/env bash
# Provision native AFL++ GCC-plugin instrumentation for the pkt.systems lifecycle.
set -euo pipefail

version=5.02c
revision=1
archive_name="AFLplusplus-${version}.tar.gz"
archive_sha256=118415843e5d289d63bd6d8f2252c18212978f15ac9e86acbbc75766cd45acde
skill_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
bootlin="$skill_dir/scripts/cpkt-toolchains.sh"
die() { printf 'cpkt-aflpp: %s\n' "$*" >&2; exit 1; }
cache() {
  if [[ -n "${CPKT_TOOLCHAIN_CACHE:-}" ]]; then printf '%s\n' "$CPKT_TOOLCHAIN_CACHE"
  elif [[ -n "${XDG_CACHE_HOME:-}" ]]; then printf '%s/c.pkt.systems/toolchains\n' "$XDG_CACHE_HOME"
  elif [[ -n "${HOME:-}" ]]; then printf '%s/.cache/c.pkt.systems/toolchains\n' "$HOME"
  else die 'HOME, XDG_CACHE_HOME, or CPKT_TOOLCHAIN_CACHE is required'; fi
}
root() { printf '%s/roots/aflplusplus-%s-x86_64-linux-gnu\n' "$(cache)" "$version"; }
value() { sed -n "s/^$1=//p" <<<"$2" | tail -1; }
ready() {
  local r=$1
  [[ -x "$r/bin/afl-fuzz" && -x "$r/bin/cpkt-afl-gcc" && -x "$r/bin/cpkt-afl-g++" &&
     -f "$r/lib/afl/afl-gcc-pass.so" && -f "$r/lib/afl/afl-compiler-rt.o" &&
     -f "$r/.cpkt-aflpp-revision-$revision" ]]
}

ensure() {
  [[ "$(uname -s)" = Linux ]] || die 'AFL++ GCC-plugin fuzzing is native Linux-only'
  case "$(uname -m)" in x86_64|amd64) ;; *) die "native x86_64 Linux is required; no cross, emulator, or QEMU runner is supported";; esac
  local r c archive desc cc cxx br tmp src dl
  r=$(root); c=$(cache); archive="$c/archives/$archive_name"
  ready "$r" && return
  [[ -x "$bootlin" ]] || die "Bootlin resolver missing: $bootlin"
  "$bootlin" ensure x86_64-linux-gnu >/dev/null
  desc=$("$bootlin" discover x86_64-linux-gnu)
  cc=$(value cc "$desc"); cxx=$(value cxx "$desc"); br=$(value root "$desc")
  [[ -x "$cc" && -x "$cxx" && -f "$br/include/gmp.h" ]] || die 'Bootlin GCC plugin headers are incomplete'
  mkdir -p "$c/archives"
  if ! [[ -f "$archive" ]] || ! printf '%s  %s\n' "$archive_sha256" "$archive" | sha256sum -c - >/dev/null 2>&1; then
    rm -f "$archive"; dl="$archive.tmp.$$"
    if command -v curl >/dev/null; then
      curl -fL --retry 3 --connect-timeout 20 -o "$dl" "https://github.com/AFLplusplus/AFLplusplus/archive/refs/tags/v${version}.tar.gz" || { rm -f "$dl"; die 'AFL++ download failed'; }
    elif command -v wget >/dev/null; then
      wget -O "$dl" "https://github.com/AFLplusplus/AFLplusplus/archive/refs/tags/v${version}.tar.gz" || { rm -f "$dl"; die 'AFL++ download failed'; }
    else die 'curl or wget is required to download AFL++'; fi
    printf '%s  %s\n' "$archive_sha256" "$dl" | sha256sum -c - >/dev/null || { rm -f "$dl"; die 'AFL++ checksum mismatch'; }
    mv "$dl" "$archive"
  fi
  tmp="$c/.aflplusplus.$$"; trap 'rm -rf "${tmp:-}"' EXIT HUP INT TERM
  mkdir -p "$tmp/extract" "$tmp/root/bin" "$tmp/root/lib/afl"
  tar -xzf "$archive" -C "$tmp/extract"; src="$tmp/extract/AFLplusplus-$version"
  [[ -d "$src" ]] || die "unexpected archive layout: $archive_name"
  (
    cd "$src"; local helper="$r/lib/afl"
    make -j1 NO_PYTHON=1 CC="$cc" CXX="$cxx" PREFIX="$tmp/root" HELPER_PATH="$helper" BIN_PATH="$tmp/root/bin" afl-fuzz afl-showmap afl-tmin afl-gotcpu afl-analyze afl-cmin
    "$cc" -O3 -funroll-loops -fPIC -Wall -g -Iinclude -Iinstrumentation -DAFL_PATH=\"$helper\" -DBIN_PATH=\"$r/bin\" -DLLVM_BINDIR=\"\" -DVERSION=\"++$version\" -DLLVM_LIBDIR=\"\" -DLLVM_VERSION=\"\" -DAFL_CLANG_FLTO=\"\" -DAFL_REAL_LD=\"\" -DAFL_CLANG_LDPATH=\"\" -DAFL_CLANG_FUSELD=\"\" -DCLANG_BIN=\"$cc\" -DCLANGPP_BIN=\"$cxx\" -DUSE_BINDIR=1 -Wno-unused-function -Wno-deprecated -c src/afl-common.c -o instrumentation/afl-common.o
    "$cc" -O3 -funroll-loops -fPIC -Wall -g -Iinclude -Iinstrumentation -DAFL_PATH=\"$helper\" -DBIN_PATH=\"$r/bin\" -DLLVM_BINDIR=\"\" -DVERSION=\"++$version\" -DLLVM_LIBDIR=\"\" -DLLVM_VERSION=\"\" -DAFL_CLANG_FLTO=\"\" -DAFL_REAL_LD=\"\" -DAFL_CLANG_LDPATH=\"\" -DAFL_CLANG_FUSELD=\"\" -DCLANG_BIN=\"$cc\" -DCLANGPP_BIN=\"$cxx\" -DUSE_BINDIR=1 -Wno-unused-function -Wno-deprecated -DAFL_INCLUDE_PATH=\"$r/include/afl\" src/afl-cc.c instrumentation/afl-common.o -o afl-cc -DLLVM_MINOR=0 -DLLVM_MAJOR=0 -DCFLAGS_OPT=\"\" -lm
    ln -sf afl-cc afl-gcc-fast; ln -sf afl-cc afl-g++-fast
    make -j1 -f GNUmakefile.gcc_plugin CC="$cc" CXX="$cxx" PREFIX="$tmp/root" HELPER_PATH="$helper" BIN_PATH="$tmp/root/bin" CXXFLAGS="-O3 -g -funroll-loops -I$br/include" LDFLAGS="-L$br/lib -Wl,-rpath,$br/lib"
    install -m755 afl-fuzz afl-showmap afl-tmin afl-gotcpu afl-analyze afl-cmin afl-cc "$tmp/root/bin/"
    ln -sf afl-cc "$tmp/root/bin/afl-gcc-fast"; ln -sf afl-cc "$tmp/root/bin/afl-g++-fast"
    install -m755 afl-gcc-pass.so afl-gcc-cmplog-pass.so afl-gcc-cmptrs-pass.so "$tmp/root/lib/afl/"; install -m644 afl-compiler-rt.o dynamic_list.txt "$tmp/root/lib/afl/"
  )
  printf '#!/usr/bin/env bash\nexport AFL_PATH=%q\nexport AFL_CC=%q\nexec %q "$@"\n' "$r/lib/afl" "$cc" "$r/bin/afl-gcc-fast" > "$tmp/root/bin/cpkt-afl-gcc"
  printf '#!/usr/bin/env bash\nexport AFL_PATH=%q\nexport AFL_CC=%q\nexport AFL_CXX=%q\nexec %q "$@"\n' "$r/lib/afl" "$cc" "$cxx" "$r/bin/afl-g++-fast" > "$tmp/root/bin/cpkt-afl-g++"
  chmod +x "$tmp/root/bin/cpkt-afl-gcc" "$tmp/root/bin/cpkt-afl-g++"; touch "$tmp/root/.cpkt-aflpp-revision-$revision"
  ready "$tmp/root" || die 'incomplete AFL++ build'; rm -rf "$r"; mv "$tmp/root" "$r"; trap - EXIT HUP INT TERM; rm -rf "$tmp"
}

report() { ensure; local r=$(root); printf 'version=%s\ncache=%s\nsource=aflplusplus\nroot=%s\nafl_fuzz=%s\nafl_showmap=%s\ncc=%s\ncxx=%s\nhelper=%s\n' "$version" "$(cache)" "$r" "$r/bin/afl-fuzz" "$r/bin/afl-showmap" "$r/bin/cpkt-afl-gcc" "$r/bin/cpkt-afl-g++" "$r/lib/afl"; }
env_out() { local d cc cxx r; ensure; d=$("$bootlin" discover x86_64-linux-gnu); cc=$(value cc "$d"); cxx=$(value cxx "$d"); r=$(root); printf 'export CPKT_AFLPP_ROOT=%q\nexport AFL_PATH=%q\nexport AFL_CC=%q\nexport AFL_CXX=%q\nexport CC=%q\nexport CXX=%q\n' "$r" "$r/lib/afl" "$cc" "$cxx" "$r/bin/cpkt-afl-gcc" "$r/bin/cpkt-afl-g++"; }
case "${1:-}" in
  ensure) [[ $# -eq 1 ]] || die 'usage: cpkt-aflpp.sh ensure'; ensure;;
  discover) [[ $# -eq 1 ]] || die 'usage: cpkt-aflpp.sh discover'; report;;
  env) [[ $# -eq 1 ]] || die 'usage: cpkt-aflpp.sh env'; env_out;;
  *) die 'usage: cpkt-aflpp.sh {ensure|discover|env}';;
esac
