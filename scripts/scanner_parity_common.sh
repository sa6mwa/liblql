scanner_parity_init() {
  scanner_parity_name=$1
  scanner_parity_default_out=$2
  scanner_parity_default_samples=$3

  c_bench=${LQL_DIRECT_BENCH_PATH:-build/release-scanner/lql_direct_bench}
  go_bench=${LQL_GO_BENCH_PATH:-build/reference-lqlbench}
  validator=${LQL_BENCHVALIDATE_PATH:-build/reference-benchvalidate}
  out=${LQL_SCANNER_PARITY_OUT:-$scanner_parity_default_out}
  samples=${LQL_BENCH_SAMPLES:-$scanner_parity_default_samples}

  if [ ! -x "$c_bench" ]; then
    printf '%s: missing C benchmark binary: %s\n' "$scanner_parity_name" \
      "$c_bench" >&2
    exit 1
  fi
  if [ ! -x "$go_bench" ]; then
    printf '%s: missing Go benchmark binary: %s\n' "$scanner_parity_name" \
      "$go_bench" >&2
    exit 1
  fi
  if [ ! -x "$validator" ]; then
    printf '%s: missing benchmark validator: %s\n' "$scanner_parity_name" \
      "$validator" >&2
    exit 1
  fi

  mkdir -p "$(dirname "$out")"
  : >"$out"
  export LQL_BENCH_SAMPLES=$samples
}

scanner_parity_run_one() {
  impl=$1
  submode=$2
  fixture=$3
  dataset=$4
  selector=$5
  expr=$6
  mode=$7
  projection=$8

  if [ ! -f "$fixture" ]; then
    printf '%s: missing fixture: %s\n' "$scanner_parity_name" "$fixture" >&2
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

scanner_parity_run_row() {
  fixture=$1
  dataset=$2
  selector=$3
  expr=$4
  mode=$5
  projection=$6

  scanner_parity_run_one go warmup_included "$fixture" "$dataset" "$selector" \
    "$expr" "$mode" "$projection"
  scanner_parity_run_one c warmup_included "$fixture" "$dataset" "$selector" \
    "$expr" "$mode" "$projection"
  scanner_parity_run_one go steady_state "$fixture" "$dataset" "$selector" \
    "$expr" "$mode" "$projection"
  scanner_parity_run_one c steady_state "$fixture" "$dataset" "$selector" \
    "$expr" "$mode" "$projection"
}

scanner_parity_validate() {
  "$validator" --forbid-unsupported --min-c-go-speedup=1.0 \
    --speedup-submode steady_state <"$out"
  printf '%s: wrote %s\n' "$scanner_parity_name" "$out"
}
