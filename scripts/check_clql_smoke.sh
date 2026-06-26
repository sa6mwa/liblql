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
short_version_out=$("$clql" -v)
if [ "$short_version_out" != "clql $version" ]; then
  printf 'clql smoke: unexpected -v output: %s\n' "$short_version_out" >&2
  printf 'clql smoke: expected: clql %s\n' "$version" >&2
  exit 1
fi

help_out=$("$clql" --help)
for needle in \
  "usage: clql" \
  "--or|-O" \
  "--compact|-c" \
  "--inline|-i|--write|-w" \
  "--enable-file-mutations|-F" \
  "--theme|-t theme" \
  "--field|-f field" \
  "--mutate|-m expr" \
  "--matches-only|-M" \
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

theme_input='{"status":"open"}'
theme_expected='{"status":"open"}'
for theme_args in \
  '-c --theme default' \
  '-c --theme=default' \
  '-c -t default' \
  '-c -tdefault' \
  '-c -t=default' \
  '-ctdefault' \
  '-ct=default'
do
  # shellcheck disable=SC2086
  theme_out=$(printf '%s' "$theme_input" | "$clql" $theme_args '/status="open"')
  if [ "$theme_out" != "$theme_expected" ]; then
    printf 'clql smoke: theme compatibility output mismatch for %s: %s\n' \
      "$theme_args" "$theme_out" >&2
    printf 'clql smoke: expected: %s\n' "$theme_expected" >&2
    exit 1
  fi
done
