#!/bin/sh
set -eu

clql=${1:?usage: check_clql_smoke.sh CLQL VERSION}
version=${2:?usage: check_clql_smoke.sh CLQL VERSION}

version_out=$("$clql" --version)
if [ "$version_out" != "clql $version" ]; then
  printf 'clql smoke: unexpected --version output: %s\n' "$version_out" >&2
  printf 'clql smoke: expected: clql %s\n' "$version" >&2
  exit 1
fi

help_out=$("$clql" --help)
for needle in \
  "usage: clql" \
  "--or|-O" \
  "--compact|-c" \
  "--field|-f field" \
  "--mutate|-m expr" \
  "--matches-only|-M" \
  "--enable-file-mutations|-F" \
  "clql --help" \
  "clql --version"
do
  case "$help_out" in
    *"$needle"*) ;;
    *)
      printf 'clql smoke: --help output missing: %s\n' "$needle" >&2
      exit 1
      ;;
  esac
done
