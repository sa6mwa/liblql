#!/bin/sh
set -eu

start=$(date +%s)

format_elapsed() {
  seconds=$1
  hours=$((seconds / 3600))
  minutes=$(((seconds % 3600) / 60))
  rest=$((seconds % 60))
  if [ "$hours" -gt 0 ]; then
    printf '%dh%02dm%02ds' "$hours" "$minutes" "$rest"
  elif [ "$minutes" -gt 0 ]; then
    printf '%dm%02ds' "$minutes" "$rest"
  else
    printf '%ds' "$rest"
  fi
}

report_elapsed() {
  status=$1
  end=$(date +%s)
  elapsed=$((end - start))
  label=failed
  if [ "$status" -eq 0 ]; then
    label=passed
  elif [ "$status" -eq 129 ] || [ "$status" -eq 130 ] ||
    [ "$status" -eq 143 ]; then
    label=interrupted
  fi
  printf 'test-all: %s in %s (%ss)\n' "$label" "$(format_elapsed "$elapsed")" \
    "$elapsed"
  exit "$status"
}

trap 'report_elapsed 129' HUP
trap 'report_elapsed 130' INT
trap 'report_elapsed 143' TERM

make_cmd=${MAKE:-make}
gates=${TEST_ALL_GATES:-}

if [ -z "$gates" ]; then
  gates=$("$make_cmd" --no-print-directory -f Makefile print-test-all-gates)
fi

for gate in $gates; do
  gate_start=$(date +%s)
  printf 'test-all: running %s\n' "$gate"
  if "$make_cmd" --no-print-directory -f Makefile "$gate"; then
    gate_end=$(date +%s)
    gate_elapsed=$((gate_end - gate_start))
    printf 'test-all: %s passed in %s (%ss)\n' "$gate" \
      "$(format_elapsed "$gate_elapsed")" "$gate_elapsed"
  else
    gate_status=$?
    gate_end=$(date +%s)
    gate_elapsed=$((gate_end - gate_start))
    printf 'test-all: %s failed in %s (%ss)\n' "$gate" \
      "$(format_elapsed "$gate_elapsed")" "$gate_elapsed"
    report_elapsed "$gate_status"
  fi
done

report_elapsed 0
