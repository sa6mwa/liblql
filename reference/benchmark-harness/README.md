# Benchmark Harness Reference

`legacy-run-parity-benchmarks.sh` is the preserved benchmark matrix for the
direct-execution rewrite. It defines deterministic fixture generation, selector
rows, benchmark modes, warmup/steady-state submodes, Go/C order alternation,
counter comparison, fixture SHA-256 checks, and root-array rejection.

It is reference-only because its C and Lua invocations target deleted code. Do
not restore those binaries or their implementation. Build a new harness that
keeps the generator, case matrix, JSONL schema, and validation rules while
calling the new direct receiver C runner.

The required C/Go matrix is `--impl go,c --check --require go,c`; every row
must be paired, have equal counters and fixture hash, and pass the `1.0x`
speed gate in `reference/go-benchmark/cmd/benchvalidate`.
