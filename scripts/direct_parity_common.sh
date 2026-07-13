direct_parity_init() {
  direct_parity_name=$1
  direct_parity_default_out=$2
  direct_parity_default_samples=$3

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
  submode=$2
  fixture=$3
  dataset=$4
  selector=$5
  expr=$6
  mode=$7
  projection=$8

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
      --mode "$mode" \
      --submode "$submode" \
      --projection-path "$projection" >>"$out"
  else
    "$c_bench" \
      --fixture "$fixture" \
      --dataset "$dataset" \
      --selector-name "$selector" \
      --expr "$expr" \
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

  direct_parity_run_one go warmup_included "$fixture" "$dataset" "$selector" \
    "$expr" "$mode" "$projection"
  direct_parity_run_one c warmup_included "$fixture" "$dataset" "$selector" \
    "$expr" "$mode" "$projection"
  direct_parity_run_one go steady_state "$fixture" "$dataset" "$selector" \
    "$expr" "$mode" "$projection"
  direct_parity_run_one c steady_state "$fixture" "$dataset" "$selector" \
    "$expr" "$mode" "$projection"
}

direct_parity_validate() {
  "$validator" --forbid-unsupported --min-c-go-speedup=1.0 \
    --speedup-submode steady_state <"$out"
  printf '%s: wrote %s\n' "$direct_parity_name" "$out"
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
