#!/bin/sh
set -eu

src=${1:?usage: check_cpp_header_smoke.sh SOURCE INCLUDE GENERATED_INCLUDE OUT}
include_dir=${2:?usage: check_cpp_header_smoke.sh SOURCE INCLUDE GENERATED_INCLUDE OUT}
generated_include_dir=${3:?usage: check_cpp_header_smoke.sh SOURCE INCLUDE GENERATED_INCLUDE OUT}
out=${4:?usage: check_cpp_header_smoke.sh SOURCE INCLUDE GENERATED_INCLUDE OUT}

if [ -n "${CXX:-}" ]; then
  cxx=$CXX
elif command -v g++ >/dev/null 2>&1; then
  cxx=g++
elif command -v c++ >/dev/null 2>&1; then
  cxx=c++
else
  printf 'SKIP: no C++ compiler available for public header smoke\n'
  exit 0
fi

"$cxx" -std=c++98 -Wall -Wextra -Wpedantic -Werror \
  -I"$include_dir" -I"$generated_include_dir" \
  -c "$src" -o "$out"
