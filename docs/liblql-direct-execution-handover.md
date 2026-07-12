# Historical LoneJSON-Backed Direct Execution Handover

This handover applies only to the rejected LoneJSON-backed design. The active
architecture is
[`liblql-self-contained-execution-spec.md`](liblql-self-contained-execution-spec.md).

## Starting Point

Start from commit `d7fc6bd` plus the specification update that accompanies
this handover. The old execution architecture was intentionally removed. Do
not search git history for implementation to reuse.

Retained code:

- `src/lql_selector.c` and `src/lql_temporal.c`: selector language data,
  parsing, AST traversal, and temporal parsing only;
- `src/lql_allocator.c`: receiver allocation boundary;
- `vendor/lonejson/lonejson.h`: the only permitted LoneJSON surface;
- `reference/go-benchmark`: pinned Go benchmark producer and validator.
- `reference/benchmark-harness`: preserved fixture generator and complete
  benchmark matrix. It is reference-only because its C/Lua calls target the
  removed architecture.

Removed code:

- Candidate Run/candidate-transform LoneJSON surface;
- old evaluator, projection executor, mutation executor, and runtime pooling;
- old C receiver stream APIs, `clql`, Lua binding, CGo bridge, C benchmark
  runner, old baselines, old parity claims, and package/release tooling.

The removal is enforced by:

```sh
./scripts/check_direct_execution_reset.sh
```

This check must keep passing. If an old symbol is needed, the correct action is
to design the direct equivalent, not whitelist or restore it.

## LoneJSON Delivery Boundary

The vendored LoneJSON source is a rewrite-time iteration harness only. It is
not the intended v0 packaging model. The final liblql binary links the
upstream LoneJSON ABI (`.a` or `.so`) so a deliverable contains one shared
LoneJSON implementation for all of its consumers. Consequently, every direct
execution design must be valid without cross-library IPO/LTO and without
assuming a particular compiler for LoneJSON. Use only public `lonejson.h`;
make vendored changes generic and upstreamable.

GCC is the authoritative performance-gate compiler. A row that wins under
Clang but loses to Go under GCC is not accepted.

## First Actions

1. Read `docs/liblql-self-contained-execution-spec.md` completely.
2. Fetch and inspect the exact Go source when needed:

   ```sh
   GOMODCACHE="$PWD/.reference-mod-cache" \
     go mod download -json pkt.systems/lql@v0.17.1
   ```

   Remove `.reference-mod-cache` after inspection; it is not a repository
   dependency.
3. Define the new receiver request/result declarations before writing any
   executor code. Keep one execution entry point and adapters only.
4. Build the direct selector slab first. Do not port a transform or action
   engine.
5. Restore tests as observable behavior tests around the new contract. Do not
   restore old implementation tests.

## Oracle Use

Build the Go benchmark tools once, then invoke their binaries rather than
including Go compilation in measurements:

```sh
cd reference/go-benchmark
go build -o ../../build/reference-lqlbench ./cmd/lqlbench
go build -o ../../build/reference-benchvalidate ./cmd/benchvalidate
```

The C runner and fixture generator are intentionally absent. Add them only
after the direct receiver executes real strict-NDJSON behavior. Reuse the
preserved harness's fixture and case definitions exactly, but replace its old C
invocation with the new direct-receiver runner. Both runners must emit the
shared JSONL record schema and validate with:

```sh
build/reference-benchvalidate --min-c-go-speedup=1.0 < results.jsonl
```

The validator alone does not prove the matrix is complete. The new benchmark
runner must reject missing Go/C pairs and compare counters and fixture hashes
before applying the speed gate.

## Stop Conditions

Stop and correct the design before proceeding if any proposal needs candidate
capture by default, an input/result cache, a LoneJSON private symbol, root
array flattening, a second execution path for files/CLI/Lua, or a benchmark
exception below `1.0x` Go. Those are design failures, not later optimizations.
