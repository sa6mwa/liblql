# liblql Go/C/Lua Parity Benchmark Spec

## Goal

Add an executable benchmark surface that compares the Go, C, and Lua
implementations of LQL on the same generated payloads, selector expressions,
execution modes, and result validation rules.

The benchmark is not only a speed comparison. It is also a parity gate:

- every implementation must scan the same logical candidates;
- every implementation must produce the same match counts;
- payload/capture modes must prove equivalent observable behavior;
- failures must identify which language/backend diverged and on which dataset,
  selector, and mode.

The Go implementation is the behavioral reference because `parity/go.mod`
already pins `pkt.systems/lql v0.17.1`. The C and Lua implementations live in
this repository.

## Command Surface

Add these repository targets when the C and Lua implementation surfaces exist:

- `make bench`
- `make benchmarks`
- `make bench-check`
- `make benchmarks-go`
- `make benchmarks-c`
- `make benchmarks-lua`
- `make benchmarks-parity`

`make bench` and `make benchmarks` should run the normal local comparison with
moderate dataset sizes. `make bench-check` should be suitable as a deterministic
performance/parity gate. `make benchmarks-parity` should run all three
language backends and fail on behavioral divergence even if timing collection
succeeds.

The benchmark runner should also be callable directly through one script, for
example:

```sh
scripts/run_parity_benchmarks.sh --impl go,c,lua --format json
```

The exact script name can differ, but Make must remain the public command
surface.

Current implementation status:

- `scripts/run_parity_benchmarks.sh` is the benchmark entry point.
- `make bench` and `make benchmarks` run a small deterministic development
  matrix, emit JSON Lines, and validate every result record against the
  benchmark schema.
- `make bench-check` runs the current deterministic Go/C/Lua smoke gate and
  fails if candidate, match, payload-count, or payload-byte counts diverge. It
  also runs deterministic negative checks proving candidate-count, match-count,
  payload-count, payload-byte, and missing-required-implementation failures are
  detected and validates benchmark JSON Lines records. It verifies
  deterministic fixture regeneration, every result record includes a SHA-256
  digest for its generated fixture, and every
  implementation/dataset/selector/mode tuple emits both `warmup_included` and
  `steady_state` records. The smoke gate is explicit and is not part of
  `make test-all`; `make prerelease` runs it after the normal test gate.
- `make benchmarks-c` exercises the C CLI for decision-only output and the
  public liblql API for matched-only seekable plus-value payload access over a
  shared generated fixture.
- `make benchmarks-go` exercises `parity/cmd/lqlbench`, which uses the pinned
  Go module and emits stable JSON Lines without scraping `go test` output.
  Go helper records report `ns_per_op`; `steady_state` performs one untimed
  warmup fixture pass before the measured pass. The schema validator requires
  supported Go records to report timing.
- the current executable dataset matrix covers NDJSON, top-level array, and
  single-root JSON object fixture shapes for both library-style and CLI-style
  record forms, with all generated once and shared by Go, C, and Lua.
- the current executable selector matrix covers equality, contains,
  `contains.any`, case-insensitive contains, timestamp comparison, date
  window, and numeric range terms over record-stream fixtures, plus nested
  `/records[]/...` selection over the single-root JSON fixture.
- the current executable mode matrix covers `decision_only_selector`,
  `decision_only_plan`, `plus_value_selector`, `plus_value_plan`,
  `plus_value_openjson_selector`, and `plus_value_openjson_plan`.
  Plus-value records assert equivalent payload counts and payload byte totals,
  while C exposes `seekable_range` payloads for seekable fixture files and
  `spool` payloads for callback-source open-read modes without retaining
  candidate JSON after callback scope.
  The current C plan benchmark reuses the parsed public `lql_selector` handle;
  it is a plan-shaped steady-state path, not a distinct compiled-plan API.
  The C native payload/plan helper reports `ns_per_op`, and the schema
  validator requires timing for supported C native helper modes. Shell-mediated
  C CLI selection records and Lua facade records may still report `null`
  timing.
- the current executable CLI-style selector matrix covers grouped
  equality/range, service contains, service case-insensitive contains, service
  `contains.any`, service `icontains.any`, and nested `/records[]/...`
  equivalents over the CLI-style single-root JSON fixture.
- `make benchmarks-lua` loads the initial `lua/lql.lua` facade, which
  orchestrates the public `clql` executable over the shared fixtures and emits
  stable JSON Lines records. This is a CLI-backed Lua parity runner, not yet
  the final Lua C module.
- `make benchmarks-parity` requires Go, C, and Lua benchmark implementations
  and fails on missing runners or counter divergence.

## Source Benchmark To Mirror

Use the existing Go benchmark shapes as the baseline:

- library benchmark: `BenchmarkQueryStreamSynthetic`;
- CLI benchmark: `BenchmarkLQLSelectionBaseline`;
- mode helper: `runBenchmarkModes`, with `warmup_included` and `steady_state`.

The new parity benchmark must not shell out to `go test` and scrape arbitrary
human benchmark output as its primary data model. It may invoke Go benchmark
code, but the shared comparison harness should emit a stable machine-readable
result record.

## Dataset Matrix

Generate the same deterministic datasets for all implementations:

1. `large_ndjson`
   - repeated JSON objects separated by newlines;
   - default count should mirror Go library benchmark scale:
     `LQL_BENCH_QUERY_NDJSON_COUNT`, fallback `20000`;
   - CLI-style benchmark may also support `LQL_BENCH_NDJSON_COUNT`, fallback
     `30000`.

2. `large_array`
   - one top-level JSON array of candidate objects;
   - default count should mirror Go library benchmark scale:
     `LQL_BENCH_QUERY_ARRAY_COUNT`, fallback `20000`;
   - CLI-style benchmark may also support `LQL_BENCH_ARRAY_COUNT`, fallback
     `30000`.

3. `large_single_json`
   - one top-level object containing a `records` array;
   - default count should mirror Go library benchmark scale:
     `LQL_BENCH_QUERY_SINGLE_COUNT`, fallback `12000`;
   - CLI-style benchmark may also support `LQL_BENCH_SINGLE_COUNT`, fallback
     `20000`.

Datasets must be generated once per benchmark run and consumed by all
implementations. Do not allow each language implementation to generate its own
payload independently, because small generator drift would invalidate the
comparison.

The generator should write dataset files under generated state such as
`build/bench-fixtures/` or `.cache/bench-fixtures/`, never under source control
unless explicitly producing a tiny checked-in smoke fixture.

The benchmark suite must include a large-fixture profile that proves C and Lua
can query a 1 GB JSON input while constrained to a 128 MB process memory
budget. This is the runnable gate, not the architectural ceiling: the design
must also remain valid for a 1 TB JSON input on an 8 MB embedded machine by
keeping steady-state memory independent of total input size, candidate size,
match count, and result set size. Passing this profile requires bounded
streaming behavior; a benchmark that materializes the input, candidate payloads,
or complete result set fails the requirement even if it reports correct matches.

## Record Shape

For the library-style query benchmark, records should match the Go benchmark's
synthetic record shape:

```json
{
  "id": "id-0",
  "status": "open",
  "metrics": {
    "retries": 0,
    "qps": 1
  },
  "timestamp": "2026-03-05T11:28:21+01:00",
  "blob": "xxxxxxxx..."
}
```

Rules:

- `status` cycles through `new`, `open`, `closed`, `queued`;
- `timestamp` alternates between `2026-03-05T11:28:21+01:00` and
  `2026-03-05T11:29:41.265+01:00`;
- `blob` is deterministic repeated text;
- numeric fields are deterministic functions of the record index.

For CLI-style selection benchmarks, the richer record shape from
`BenchmarkLQLSelectionBaseline` should be available:

```json
{
  "id": "id-0000000",
  "region": "us-west",
  "service": "auth-api",
  "status": "open",
  "metrics": {
    "latency_ms": 10,
    "qps": 1,
    "errors": 0
  },
  "message": "request-0 service=auth-api region=us-west",
  "tags": ["prod", "blue", "v2"]
}
```

The initial implementation may choose one record shape if it is documented and
all three implementations use it. The long-term benchmark should include both
library-style query payloads and CLI-style selection payloads because they
stress different selector paths.

## Selector Matrix

At minimum, benchmark these selectors:

- `/status="open"`
- `contains{field=/blob,value=xxxx}`
- `contains{field=/blob,any=xxxx|nomatch}`
- `icontains{field=/blob,value=XXXX}`
- `/timestamp>=2026-03-05T10:28:21Z`
- `date{field=/timestamp,after=2026-03-05T10:28:21Z,before=2026-03-05T10:29:50Z}`
- `/records[]/status="open"` for `large_single_json`

For CLI-style selection parity, also include:

- `and.eq{field=/region,value=us-west}` plus
  `and.range{field=/metrics/latency_ms,lt=350}`
- `contains{field=/service,value=auth}`
- `icontains{field=/service,value=AUTH}`
- `contains{field=/service,any=auth|search}`
- `icontains{field=/service,any=AUTH|GATEWAY}`
- nested `/records[]/...` equivalents for `large_single_json`

Each selector entry must declare whether it is expected to be supported by the
current C and Lua implementations. Unsupported selectors may be skipped only
with an explicit `unsupported` result in development runs. They must not be
silently omitted from parity output.

Once the port claims parity for a selector class, unsupported skips for that
selector class must become failures.

## Execution Modes

Mirror Go `BenchmarkQueryStreamSynthetic` modes:

1. `decision_only_selector`
   - parse selector once;
   - run query in decision-only mode;
   - count candidates and matches.

2. `decision_only_plan`
   - compile/reuse a plan when the implementation supports plans;
   - the current C benchmark treats the parsed public `lql_selector` handle as
     the reusable plan surface until liblql exposes a distinct compiled plan;
   - if an implementation has neither a reusable selector nor a separate plan
     object, report `unsupported` for this mode until implemented.

3. `plus_value_selector`
   - include callback-scoped candidate payload access;
   - verify a payload handle/source exists for every candidate callback where
     the mode promises payload access;
   - for seekable fixture files, verify payload access can be satisfied by
     candidate offset and byte size without candidate capture;
   - record candidate offsets and byte sizes as 64-bit values and fail the C
     benchmark if the range cannot be represented by the public liblql API;
   - do not copy or retain full candidate payloads in the benchmark harness.

4. `plus_value_plan`
   - plan variant of plus-value mode.
   - the current C benchmark uses the same seekable-range payload path as
     `plus_value_selector`, with the parsed public selector reused across the
     query run as the plan-like handle.

5. `plus_value_openjson_selector`
   - force a very small memory threshold for `large_single_json`;
   - read each matched/callback payload through the callback-scoped open/read
     source path, preferring seek/reread by candidate offset for seekable
     inputs and using spooled handles only for non-seekable inputs;
   - verify byte counts and cleanup behavior.

6. `plus_value_openjson_plan`
   - plan variant of seekable-range or spool/open-read mode.

Also mirror CLI benchmark parse behavior:

7. `reuse_selector`
   - parse/compile selector once before timing loop.

8. `reparse_selector_each_run`
   - include parse/compile cost in each run.

Every benchmark mode should have `warmup_included` and `steady_state`
submodes. `steady_state` must perform one untimed warmup run before timing.

## Result Validation

Each implementation and mode must report:

- implementation: `go`, `c`, or `lua`;
- dataset name;
- selector name and expression;
- mode name;
- submode: `warmup_included` or `steady_state`;
- bytes processed per iteration;
- candidate count;
- match count;
- payload count when payload mode is enabled;
- payload bytes read when payload mode opens/reads payloads;
- payload source type: seekable range, callback sink, or spool;
- elapsed time or ns/op;
- allocations or allocator counters when available;
- unsupported reason, if applicable.

Parity comparison must fail when:

- candidate counts differ;
- match counts differ;
- payload counts differ in payload modes;
- payload byte totals differ in open/read payload modes;
- an implementation reports success for a selector but another implementation
  reports parse failure;
- a previously required selector/mode reports unsupported.

The benchmark should not require allocations to match across languages. It
should record allocation data where meaningful, but behavioral parity is the
correctness gate.

## Output Format

Emit JSON Lines by default, one record per implementation/dataset/selector/mode
/submode combination.

Each record should be self-contained. Example shape:

```json
{
  "schema": "liblql.parity_benchmark.v1",
  "impl": "c",
  "dataset": "large_ndjson",
  "selector": "eq_status_open",
  "expr": "/status=\"open\"",
  "mode": "decision_only_selector",
  "submode": "steady_state",
  "bytes_per_iter": 4194304,
  "candidates": 20000,
  "matches": 5000,
  "payloads": 0,
  "payload_bytes": 0,
  "ns_per_op": 1234567,
  "allocs_per_op": null,
  "unsupported": false,
  "unsupported_reason": ""
}
```

The comparison script may also print a human summary table, but JSON Lines is
the contract for tools and regression gates.

## Lua Requirements

The Lua implementation lives in this repository and must be benchmarked through
the repository's Lua facade, not by shelling out to Go or by calling C private
test helpers directly. The initial Lua benchmark runner must go through
`lua/lql.lua`, whose current implementation shells out to the public `clql`
executable as an intentionally narrow bridge until the direct Lua C module
exists.

Lua benchmark entry points should support:

- loading a selector;
- optional reusable compiled selector/plan if the Lua API exposes one;
- streaming candidates from a dataset file;
- returning candidate and match counts;
- callback-scoped payload/open-read modes when Lua exposes payload handles.

Lua unsupported modes must be explicit result records. Silent absence is a
benchmark failure.

## C Requirements

The C benchmark must use installed/public liblql APIs where practical. It may
use a small benchmark-only executable under `bench/`, but that executable
should exercise the public API expected of downstream consumers.

The C benchmark must not use private lonejson APIs or private liblql structs to
win benchmark speed. If a public API is insufficient, update the public API
spec before implementing the benchmark.

## Go Requirements

The Go benchmark must use the pinned `pkt.systems/lql v0.17.1` module under
`parity/`.

It may reuse code derived from the upstream benchmark shape, but the generated
fixtures and emitted result records must be shared with the C and Lua runners.

The Go runner must not depend on the adjacent source checkout. It must resolve
through the Go module in `parity/`.

## Performance Gate Policy

Initial parity benchmark implementation should fail only on behavioral
divergence and benchmark runner errors.

Performance thresholds should be added only after:

- C and Lua have feature parity for the relevant selector classes;
- baseline logs exist for the local lifecycle environment;
- thresholds are expressed as ratios or guarded baselines with enough tolerance
  to avoid noisy failures.

Suggested first gate:

- Go/C/Lua behavior must match for supported selector classes;
- C must not be slower than Go by more than a documented exploratory ratio for
  decision-only steady-state on `large_ndjson`;
- Lua may be report-only until the Lua facade performance profile is known.

Any performance gate must print actionable diagnostics with dataset, selector,
mode, observed value, baseline/threshold, and reproduction command.

## Non-goals

- Do not benchmark different generated datasets per language.
- Do not compare pretty/colorized JSON output. `clql` intentionally does not
  reimplement `prettyx` colorized output.
- Do not make benchmark success depend on exact allocation equivalence across
  languages.
- Do not hide unsupported selector classes by filtering them out.
- Do not call a benchmark streaming if it materializes the full dataset first.
- Do not satisfy payload modes by retaining complete candidate copies outside
  the callback lifetime.
- Do not pass the 1 GB / 128 MB profile by increasing memory limits, using
  temporary files as an undisclosed full-input staging substitute, or disabling
  payload modes that the profile requires.
- Do not treat the 1 GB / 128 MB profile as permission for memory growth
  proportional to input size; it is only the practical CI-sized proxy for the
  1 TB / 8 MB stress model.

## Acceptance Tests

Add tests or smoke gates proving:

- fixture generation is deterministic;
- Go, C, and Lua consume the same fixture files;
- JSON Lines result records validate against the expected schema;
- comparison fails on an injected match-count mismatch;
- comparison fails on an injected candidate-count mismatch;
- unsupported modes are represented explicitly;
- the large-fixture profile fails when the implementation materializes the
  dataset or complete candidates;
- `make benchmarks-parity` fails if any required implementation is missing;
- `make bench-check` runs a small deterministic matrix suitable for local
  confidence.
