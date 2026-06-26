#!/bin/sh
set -eu

if [ "${1:-}" = "--fixtures" ]; then
  tmp_fixture=${TMPDIR:-/tmp}/liblql-sdk-manifest-fixtures.$$
  trap 'rm -rf "$tmp_fixture"' EXIT HUP INT TERM
  mkdir -p "$tmp_fixture"
  fixture_header=$tmp_fixture/lql.h
  fixture_test=$tmp_fixture/test_lql.c

  cat >"$fixture_header" <<'EOF'
typedef struct lql lql;
struct lql {
  void *impl;
  void (*alpha)(lql *self);
  void (*destroy)(lql *self);
};
EOF

  cat >"$fixture_test" <<'EOF'
static void expect_alpha_api(void) {
  lql *ctx = 0;
  ctx->alpha(ctx);
}
static void expect_destroy_api(void) {
  lql *ctx = 0;
  ctx->destroy(ctx);
}
static void expect_sdk_contract_manifest(void) {
  static const int manifest[] = {
      (int)(long)expect_alpha_api,
      (int)(long)expect_destroy_api,
  };
  (void)manifest;
}
int main(void) {
  expect_alpha_api();
  expect_destroy_api();
  return 0;
}
EOF

  if ! sh "$0" "$fixture_test" "$fixture_header"; then
    printf 'SDK unit manifest fixture: expected clean fixture to pass\n' >&2
    exit 1
  fi

  cat >"$fixture_header" <<'EOF'
typedef struct lql lql;
struct lql {
  void *impl;
  void (*alpha)(lql *self);
  void (*beta)(lql *self);
  void (*destroy)(lql *self);
};
EOF
  if sh "$0" "$fixture_test" "$fixture_header" >/dev/null 2>&1; then
    printf 'SDK unit manifest fixture: expected missing receiver method coverage to fail\n' >&2
    exit 1
  fi

  cat >"$fixture_header" <<'EOF'
typedef struct lql lql;
struct lql {
  void *impl;
  void (*alpha)(lql *self);
  void (*destroy)(lql *self);
};
EOF
  cat >"$fixture_test" <<'EOF'
static void expect_alpha_api(void) {
  lql *ctx = 0;
  ctx->alpha(ctx);
}
static void expect_destroy_api(void) {
  lql *ctx = 0;
  ctx->destroy(ctx);
}
static void expect_extra_api(void) {
}
static void expect_sdk_contract_manifest(void) {
  static const int manifest[] = {
      (int)(long)expect_alpha_api,
      (int)(long)expect_destroy_api,
  };
  (void)manifest;
}
int main(void) {
  expect_alpha_api();
  expect_destroy_api();
  expect_extra_api();
  return 0;
}
EOF
  if sh "$0" "$fixture_test" "$fixture_header" >/dev/null 2>&1; then
    printf 'SDK unit manifest fixture: expected unmanifested SDK unit to fail\n' >&2
    exit 1
  fi

  cat >"$fixture_test" <<'EOF'
static void expect_alpha_api(void) {
  lql *ctx = 0;
  ctx->alpha(ctx);
}
static void expect_destroy_api(void) {
  lql *ctx = 0;
  ctx->destroy(ctx);
}
static void expect_sdk_contract_manifest(void) {
  static const int manifest[] = {
      (int)(long)expect_alpha_api,
      (int)(long)expect_alpha_api,
      (int)(long)expect_destroy_api,
  };
  (void)manifest;
}
int main(void) {
  expect_alpha_api();
  expect_destroy_api();
  return 0;
}
EOF
  if sh "$0" "$fixture_test" "$fixture_header" >/dev/null 2>&1; then
    printf 'SDK unit manifest fixture: expected duplicate manifest entry to fail\n' >&2
    exit 1
  fi

  cat >"$fixture_test" <<'EOF'
typedef void (*sdk_contract_test_fn)(void);
typedef struct sdk_contract_requirement {
  const char *surface;
  const char *requirement;
  sdk_contract_test_fn test;
} sdk_contract_requirement;
static void expect_alpha_api(void) {
  lql *ctx = 0;
  ctx->alpha(ctx);
}
static void expect_destroy_api(void) {
  lql *ctx = 0;
  ctx->destroy(ctx);
}
static void expect_sdk_contract_manifest(void) {
  static const sdk_contract_requirement manifest[] = {
      {"receiver", "duplicate requirement", expect_alpha_api},
      {"receiver", "duplicate requirement", expect_destroy_api},
  };
  (void)manifest;
}
int main(void) {
  expect_alpha_api();
  expect_destroy_api();
  return 0;
}
EOF
  if sh "$0" "$fixture_test" "$fixture_header" >/dev/null 2>&1; then
    printf 'SDK unit manifest fixture: expected duplicate requirement key to fail\n' >&2
    exit 1
  fi

  cat >"$fixture_test" <<'EOF'
static void expect_alpha_api(void) {
  lql *ctx = 0;
  ctx->alpha(ctx);
}
static void expect_destroy_api(void) {
  lql *ctx = 0;
  ctx->destroy(ctx);
}
static void expect_sdk_contract_manifest(void) {
  static const int manifest[] = {
      (int)(long)expect_alpha_api,
      (int)(long)expect_destroy_api,
  };
  (void)manifest;
}
int main(void) {
  expect_alpha_api();
  expect_alpha_api();
  expect_destroy_api();
  return 0;
}
EOF
  if sh "$0" "$fixture_test" "$fixture_header" >/dev/null 2>&1; then
    printf 'SDK unit manifest fixture: expected duplicate main call to fail\n' >&2
    exit 1
  fi

  cat >"$fixture_test" <<'EOF'
static void expect_alpha_api(void) {
}
static void expect_destroy_api(void) {
  lql *ctx = 0;
  ctx->destroy(ctx);
}
static void expect_sdk_contract_manifest(void) {
  static const int manifest[] = {
      (int)(long)expect_alpha_api,
      (int)(long)expect_destroy_api,
  };
  (void)manifest;
}
int main(void) {
  lql *ctx = 0;
  ctx->alpha(ctx);
  expect_alpha_api();
  expect_destroy_api();
  return 0;
}
EOF
  if sh "$0" "$fixture_test" "$fixture_header" >/dev/null 2>&1; then
    printf 'SDK unit manifest fixture: expected receiver call outside SDK unit to fail\n' >&2
    exit 1
  fi

  exit 0
fi

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  printf 'usage: %s tests/test_lql.c [include/lql/lql.h]\n' "$0" >&2
  exit 2
fi

test_file=$1
header_file=${2:-}
if [ ! -f "$test_file" ]; then
  printf 'SDK unit manifest check: missing test file: %s\n' "$test_file" >&2
  exit 2
fi
if [ -n "$header_file" ] && [ ! -f "$header_file" ]; then
  printf 'SDK unit manifest check: missing public header: %s\n' "$header_file" >&2
  exit 2
fi

tmp_dir=${TMPDIR:-/tmp}/liblql-sdk-manifest.$$
mkdir -p "$tmp_dir"
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

functions_file=$tmp_dir/functions
manifest_file=$tmp_dir/manifest
manifest_all_file=$tmp_dir/manifest-all
requirement_keys_file=$tmp_dir/requirement-keys
main_file=$tmp_dir/main
main_all_file=$tmp_dir/main-all
methods_file=$tmp_dir/receiver-methods
missing_methods_file=$tmp_dir/missing-receiver-methods
unit_method_calls_file=$tmp_dir/unit-method-calls

sed -n 's/^static void \(expect_[A-Za-z0-9_]*\)(void) {$/\1/p' "$test_file" |
  grep -v '^expect_sdk_contract_manifest$' |
  sort > "$functions_file"

awk '
  /static const .*manifest.*=[[:space:]]*\{/ { in_manifest = 1; line = ""; next }
  in_manifest && /};/ { in_manifest = 0 }
  in_manifest {
    while (match($0, /expect_[A-Za-z0-9_]*/)) {
      print substr($0, RSTART, RLENGTH)
      $0 = substr($0, RSTART + RLENGTH)
    }
  }
' "$test_file" | grep -v '^expect_sdk_contract_manifest$' | sort > "$manifest_all_file"
sort -u "$manifest_all_file" > "$manifest_file"

awk '
  /static const .*manifest.*=[[:space:]]*\{/ { in_manifest = 1; line = ""; next }
  in_manifest && /};/ { in_manifest = 0 }
  in_manifest {
    line = line " " $0
    if ($0 ~ /}/) {
      entry = line
      line = ""
      if (entry ~ /[{][[:space:]]*"/) {
        sub(/^[^{]*[{][[:space:]]*"/, "", entry)
        surface = entry
        sub(/".*$/, "", surface)
        sub(/^[^"]*"[[:space:]]*,[[:space:]]*"/, "", entry)
        requirement = entry
        sub(/".*$/, "", requirement)
        if (surface != "" && requirement != "") {
          print surface "/" requirement
        }
      }
    }
  }
' "$test_file" | sort > "$requirement_keys_file"

if [ -s "$requirement_keys_file" ] &&
   [ "$(wc -l < "$requirement_keys_file" | tr -d ' ')" != \
     "$(sort -u "$requirement_keys_file" | wc -l | tr -d ' ')" ]; then
  printf 'SDK unit manifest check: manifest lists one or more requirement keys more than once\n' >&2
  printf '%s\n' '--- duplicate requirement keys ---' >&2
  sort "$requirement_keys_file" | uniq -d >&2
  exit 1
fi

awk '
  /^int main\(void\)/ { in_main = 1 }
  in_main && /^}/ { in_main = 0 }
  in_main {
    while (match($0, /expect_[A-Za-z0-9_]*/)) {
      print substr($0, RSTART, RLENGTH)
      $0 = substr($0, RSTART + RLENGTH)
    }
  }
' "$test_file" | grep -v '^expect_sdk_contract_manifest$' | sort > "$main_all_file"
sort -u "$main_all_file" > "$main_file"

if [ ! -s "$functions_file" ]; then
  printf 'SDK unit manifest check: no SDK unit functions found\n' >&2
  exit 1
fi

if [ "$(wc -l < "$manifest_all_file" | tr -d ' ')" != \
     "$(wc -l < "$manifest_file" | tr -d ' ')" ]; then
  printf 'SDK unit manifest check: manifest lists one or more SDK unit functions more than once\n' >&2
  printf '%s\n' '--- duplicate manifest entries ---' >&2
  sort "$manifest_all_file" | uniq -d >&2
  exit 1
fi

if ! cmp -s "$functions_file" "$manifest_file"; then
  printf 'SDK unit manifest check: manifest does not match SDK unit functions\n' >&2
  printf '%s\n' '--- expected manifest entries ---' >&2
  cat "$functions_file" >&2
  printf '%s\n' '--- actual manifest entries ---' >&2
  cat "$manifest_file" >&2
  exit 1
fi

if [ "$(wc -l < "$main_all_file" | tr -d ' ')" != \
     "$(wc -l < "$main_file" | tr -d ' ')" ]; then
  printf 'SDK unit manifest check: main calls one or more SDK unit functions more than once\n' >&2
  printf '%s\n' '--- duplicate main calls ---' >&2
  sort "$main_all_file" | uniq -d >&2
  exit 1
fi

if ! cmp -s "$functions_file" "$main_file"; then
  printf 'SDK unit manifest check: main does not call every SDK unit function\n' >&2
  printf '%s\n' '--- expected main calls ---' >&2
  cat "$functions_file" >&2
  printf '%s\n' '--- actual main calls ---' >&2
  cat "$main_file" >&2
  exit 1
fi

if [ -n "$header_file" ]; then
  awk '
    /struct lql[[:space:]]*\{/ { in_receiver = 1; next }
    in_receiver && /^};/ { in_receiver = 0; next }
    in_receiver {
      if (match($0, /\(\*[A-Za-z_][A-Za-z0-9_]*\)/)) {
        name = substr($0, RSTART + 2, RLENGTH - 3)
        print name
      }
    }
  ' "$header_file" | sort -u > "$methods_file"

  awk '
    /^static void expect_[A-Za-z0-9_]*\(void\) \{$/ {
      in_unit = ($0 !~ /expect_sdk_contract_manifest/)
      next
    }
    in_unit && /^}/ { in_unit = 0; next }
    in_unit {
      line = $0
      while (match(line, /->[[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(/)) {
        call = substr(line, RSTART, RLENGTH)
        sub(/^->[[:space:]]*/, "", call)
        sub(/[[:space:]]*\($/, "", call)
        print call
        line = substr(line, RSTART + RLENGTH)
      }
    }
  ' "$test_file" | sort -u > "$unit_method_calls_file"

  : > "$missing_methods_file"
  while IFS= read -r method; do
    if ! grep -Fxq "$method" "$unit_method_calls_file"; then
      printf '%s\n' "$method" >> "$missing_methods_file"
    fi
  done < "$methods_file"

  if [ -s "$missing_methods_file" ]; then
    printf 'SDK unit manifest check: receiver methods lack C-side SDK unit method-call coverage\n' >&2
    cat "$missing_methods_file" >&2
    exit 1
  fi
fi
