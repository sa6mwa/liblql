#!/bin/sh
set -eu

inventory=parity/oracle_inventory.tsv

if [ ! -f "$inventory" ]; then
  printf 'selector AST contract: missing %s\n' "$inventory" >&2
  exit 1
fi

header=$(sed -n '1p' "$inventory")
if [ "$header" != 'area	status	evidence	notes' ]; then
  printf 'selector AST contract: unexpected oracle inventory header\n' >&2
  exit 1
fi

awk -F '\t' '
  NR == 1 { next }
  NF != 4 {
    printf "selector AST contract: %s:%d: expected 4 tab-separated fields, got %d\n", FILENAME, NR, NF > "/dev/stderr"
    bad = 1
    next
  }
  $1 == "" || $2 == "" || $3 == "" || $4 == "" {
    printf "selector AST contract: %s:%d: empty inventory field\n", FILENAME, NR > "/dev/stderr"
    bad = 1
  }
  $2 == "gap" || $2 == "partial" {
    printf "selector AST contract: %s:%d: unresolved status %s\n", FILENAME, NR, $2 > "/dev/stderr"
    bad = 1
  }
  $2 != "covered" && $2 != "intentional-divergence" && $2 != "not-supported-approved" {
    printf "selector AST contract: %s:%d: unknown status %s\n", FILENAME, NR, $2 > "/dev/stderr"
    bad = 1
  }
  END { exit bad ? 1 : 0 }
' "$inventory"

{
  read -r _header
  while IFS='	' read -r area _status evidence _notes; do
    old_ifs=$IFS
    IFS=';'
    set -- $evidence
    IFS=$old_ifs
    for raw_item do
      item=$(printf '%s' "$raw_item" |
        sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
      case "$item" in
        make\ *)
          target=${item#make }
          if ! grep -E "^${target}:" Makefile >/dev/null; then
            printf 'selector AST contract: %s: missing evidence target: %s\n' \
              "$area" "$item" >&2
            exit 1
          fi
          ;;
        *)
          path=${item%%:*}
          if [ ! -e "$path" ]; then
            printf 'selector AST contract: %s: missing evidence path: %s\n' \
              "$area" "$item" >&2
            exit 1
          fi
          ;;
      esac
    done
  done
} <"$inventory"

private_matches=$(git grep -n -E '(^|[^[:alnum:]_])(lql_node|lql_term|LQL_NODE_[[:alnum:]_]+)([^[:alnum:]_]|$)' -- \
  include src tests tools lua examples CMakeLists.txt || true)
if [ -n "$private_matches" ]; then
  printf 'selector AST contract: forbidden private AST vocabulary found:\n' >&2
  printf '%s\n' "$private_matches" >&2
  exit 1
fi

if ! grep -E '^sdk-parity-gate: direct-bench$' Makefile >/dev/null; then
  printf 'selector AST contract: sdk-parity-gate target is missing\n' >&2
  exit 1
fi

if ! grep -F 'add_test(NAME lql.selector_ast' CMakeLists.txt >/dev/null; then
  printf 'selector AST contract: lql.selector_ast CTest registration is missing\n' >&2
  exit 1
fi

printf 'selector AST contract passed\n'
