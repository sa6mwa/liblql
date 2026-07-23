direct_parity_init() {
  direct_parity_name=$1
  direct_parity_default_out=$2
  direct_parity_default_samples=$3
  direct_parity_start=$(date +%s)

  c_bench=${LQL_DIRECT_BENCH_PATH:-build/release/lql_direct_bench}
  go_bench=${LQL_GO_BENCH_PATH:-build/reference-lqlbench}
  validator=${LQL_BENCHVALIDATE_PATH:-build/reference-benchvalidate}
  out=${LQL_DIRECT_PARITY_OUT:-$direct_parity_default_out}
  samples=${LQL_BENCH_SAMPLES:-$direct_parity_default_samples}

  if [ ! -x "$c_bench" ]; then
    printf '%s: missing C benchmark binary: %s\n' "$direct_parity_name" \
      "$c_bench" >&2
    exit 1
  fi
  if [ ! -x "$go_bench" ]; then
    printf '%s: missing Go benchmark binary: %s\n' "$direct_parity_name" \
      "$go_bench" >&2
    exit 1
  fi
  if [ ! -x "$validator" ]; then
    printf '%s: missing benchmark validator: %s\n' "$direct_parity_name" \
      "$validator" >&2
    exit 1
  fi

  mkdir -p "$(dirname "$out")"
  : >"$out"
  export LQL_BENCH_SAMPLES=$samples
}

direct_parity_run_one() {
  impl=$1
  selector_format=$2
  submode=$3
  fixture=$4
  dataset=$5
  selector=$6
  expr=$7
  mode=$8
  projection=$9
  selector_json_flag=

  case "$selector_format" in
    text) ;;
    json) selector_json_flag=--selector-json ;;
    *)
      printf '%s: unknown selector format: %s\n' "$direct_parity_name" \
        "$selector_format" >&2
      exit 1
      ;;
  esac

  if [ ! -f "$fixture" ]; then
    printf '%s: missing fixture: %s\n' "$direct_parity_name" "$fixture" >&2
    exit 1
  fi

  if [ "$impl" = go ]; then
    "$go_bench" \
      --fixture "$fixture" \
      --dataset "$dataset" \
      --selector-name "$selector" \
      --expr "$expr" \
      $selector_json_flag \
      --mode "$mode" \
      --submode "$submode" \
      --projection-path "$projection" >>"$out"
  else
    "$c_bench" \
      --fixture "$fixture" \
      --dataset "$dataset" \
      --selector-name "$selector" \
      --expr "$expr" \
      $selector_json_flag \
      --mode "$mode" \
      --submode "$submode" \
      --projection-path "$projection" >>"$out"
  fi
}

direct_parity_run_row() {
  fixture=$1
  dataset=$2
  selector=$3
  expr=$4
  mode=$5
  projection=$6

  direct_parity_run_one go text warmup_included "$fixture" "$dataset" \
    "$selector" "$expr" "$mode" "$projection"
  direct_parity_run_one c text warmup_included "$fixture" "$dataset" \
    "$selector" "$expr" "$mode" "$projection"
  direct_parity_run_one go text steady_state "$fixture" "$dataset" "$selector" \
    "$expr" "$mode" "$projection"
  direct_parity_run_one c text steady_state "$fixture" "$dataset" "$selector" \
    "$expr" "$mode" "$projection"
}

direct_parity_run_json_row() {
  fixture=$1
  dataset=$2
  selector=$3
  expr=$4
  mode=$5
  projection=$6

  direct_parity_run_one go json warmup_included "$fixture" "$dataset" \
    "$selector" "$expr" "$mode" "$projection"
  direct_parity_run_one c json warmup_included "$fixture" "$dataset" \
    "$selector" "$expr" "$mode" "$projection"
  direct_parity_run_one go json steady_state "$fixture" "$dataset" "$selector" \
    "$expr" "$mode" "$projection"
  direct_parity_run_one c json steady_state "$fixture" "$dataset" "$selector" \
    "$expr" "$mode" "$projection"
}

direct_parity_expect_failure_one() {
  impl=$1
  selector_format=$2
  fixture=$3
  dataset=$4
  selector=$5
  expr=$6
  mode=$7
  projection=$8
  selector_json_flag=

  case "$selector_format" in
    text) ;;
    json) selector_json_flag=--selector-json ;;
    *)
      printf '%s: unknown selector format: %s\n' "$direct_parity_name" \
        "$selector_format" >&2
      exit 1
      ;;
  esac

  if [ "$impl" = go ]; then
    if "$go_bench" \
      --fixture "$fixture" \
      --dataset "$dataset" \
      --selector-name "$selector" \
      --expr "$expr" \
      $selector_json_flag \
      --mode "$mode" \
      --submode steady_state \
      --projection-path "$projection" >/dev/null 2>&1; then
      printf '%s: expected Go selector failure: %s\n' \
        "$direct_parity_name" "$selector" >&2
      exit 1
    fi
  else
    if "$c_bench" \
      --fixture "$fixture" \
      --dataset "$dataset" \
      --selector-name "$selector" \
      --expr "$expr" \
      $selector_json_flag \
      --mode "$mode" \
      --submode steady_state \
      --projection-path "$projection" >/dev/null 2>&1; then
      printf '%s: expected C selector failure: %s\n' \
        "$direct_parity_name" "$selector" >&2
      exit 1
    fi
  fi
}

direct_parity_expect_failure() {
  selector_format=$1
  fixture=$2
  dataset=$3
  selector=$4
  expr=$5
  mode=$6
  projection=$7

  direct_parity_expect_failure_one go "$selector_format" "$fixture" "$dataset" \
    "$selector" "$expr" "$mode" "$projection"
  direct_parity_expect_failure_one c "$selector_format" "$fixture" "$dataset" \
    "$selector" "$expr" "$mode" "$projection"
}

direct_parity_validate() {
  direct_parity_min_speedup=${LQL_DIRECT_PARITY_MIN_C_GO_SPEEDUP-1.0}
  direct_parity_require_rss=${LQL_DIRECT_PARITY_REQUIRE_C_RSS_BELOW_GO-1}
  direct_parity_validator_args="--forbid-unsupported --require-counter-parity --speedup-submode steady_state"
  if [ -n "$direct_parity_min_speedup" ]; then
    direct_parity_validator_args="$direct_parity_validator_args --min-c-go-speedup=$direct_parity_min_speedup"
  fi
  if [ "$direct_parity_require_rss" != 0 ]; then
    direct_parity_validator_args="$direct_parity_validator_args --require-c-rss-below-go"
  fi
  # direct_parity_validator_args is assembled from fixed flags plus numeric
  # gate variables owned by the repository scripts.
  # shellcheck disable=SC2086
  "$validator" $direct_parity_validator_args <"$out"
  direct_parity_end=$(date +%s)
  direct_parity_elapsed=$((direct_parity_end - direct_parity_start))
  printf '%s: wrote %s in %ss with %s sample(s)\n' "$direct_parity_name" \
    "$out" "$direct_parity_elapsed" "$samples"
}

direct_parity_expect_records() {
  expected=$1
  actual=$(wc -l <"$out" | tr -d ' ')
  if [ "$actual" != "$expected" ]; then
    printf '%s: expected %s benchmark records, got %s in %s\n' \
      "$direct_parity_name" "$expected" "$actual" "$out" >&2
    exit 1
  fi
}
