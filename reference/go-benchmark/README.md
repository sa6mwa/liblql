# Go Benchmark Oracle

This directory preserves the Go benchmark producer and JSONL validator for the
direct-execution rewrite. It pins `pkt.systems/lql v0.17.1`; it does not contain
or depend on the deleted C executor.

Build `cmd/lqlbench` and `cmd/benchvalidate` before measuring C. The new C
runner must emit the same JSONL schema and must pass `benchvalidate` with
`--min-c-go-speedup=1.0` for every accepted Go/C row.

Fixture generation and the C runner are intentionally new work. Do not recover
the deleted old C benchmark implementation. The exact fixture and row matrix is
preserved in `../benchmark-harness/legacy-run-parity-benchmarks.sh`.
