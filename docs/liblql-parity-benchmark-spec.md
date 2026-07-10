# liblql Go/C/Lua Parity Benchmark Spec

## Goal

Add an executable benchmark surface that runs the Go, C, and Lua
implementations of LQL on the same generated payloads, selector expressions,
execution modes, and result validation rules. The comparison is behavioral.
It must not define Go throughput as the acceptable C target.

The benchmark has two separate responsibilities. First, it is a behavioral
oracle gate:

- every implementation must scan the same logical candidates;
- every implementation must produce the same match counts;
- payload/capture modes must prove equivalent observable behavior;
- failures must identify which language/backend diverged and on which dataset,
  selector, and mode.

Second, it is a C-native performance and memory gate. The Go implementation is
the semantic reference, not the speed target. A mature public liblql path that
only matches Go throughput is a red flag unless the case is dominated by
documented external costs such as process startup or disk I/O. Library-level C
selector, projection, mutation, and payload access paths should normally have
substantial headroom over Go while keeping steady-state memory independent of
input size, candidate size, match count, and result size. The C gate must be
expressed as a C-native baseline, memory envelope, or speedup floor for a
specific mature public path; it must not pass just because C is no slower than
Go.

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
behavioral and C-native performance smoke gate once the relevant surfaces are
mature. `make benchmarks-parity` should run all three language backends and
fail on behavioral divergence even if timing collection succeeds. Passing
`make benchmarks-parity` proves counter agreement only; it is not the final
C performance claim.

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
  builds `debug-lua` for the Lua facade and
  `build/bench-release/lql_payload_bench` for optimized C payload/performance
  rows; debug helpers are not authoritative for C-native performance gates. It
  also runs deterministic negative checks proving candidate-count, match-count,
  payload-count, payload-byte, peak-RSS-limit, and
  missing-required-implementation failures are detected and validates benchmark
  JSON Lines records. Claimed smoke and memory gates validate with
  `benchvalidate --forbid-unsupported`, so explicit unsupported records are
  accepted only by report-only schema validation and fail gates that require
  Go/C/Lua coverage. It verifies
  deterministic fixture regeneration, every result record includes a SHA-256
  digest for its generated fixture, rejects impossible counter records such as
  `matches > candidates`, and verifies every
  implementation/dataset/selector/mode tuple emits both `warmup_included` and
  `steady_state` records. It also rejects unknown payload source type spellings
  so materialized, buffered, seekable, spooled, and Lua facade payload behavior
  cannot be blurred in benchmark output. The smoke gate also validates
  supported C records against `LQL_BENCH_MAX_C_PEAK_RSS_BYTES`, defaulting to
  128 MiB. It includes
  direct callback-source decision and plus-value selector modes so Go, C, and
  Lua exercise non-seekable source readers and callback-scoped spooled payload
  access through their public SDK surfaces. It also runs file-backed and
  callback-source mutation modes with compact matches-only output. C writes
  mutated candidates to `/dev/null` through public liblql receiver methods;
  Go writes through the pinned `QueryMutateStreamWithResult` path; Lua uses
  the public Lua facade over liblql. Mutation benchmark records compare
  candidate and match counts but deliberately leave query payload counters at
  zero because mutated output is not a query payload. This proves
  the RSS gate wiring and catches obvious materialization regressions in the
  smoke matrix; it is not the final large-fixture memory proof. The smoke gate
  is explicit and is not part of `make test-all`; `make prerelease` runs it
  after the normal test gate.
- `make bench-lockd-perf-check` runs lockd-specific Go/C performance guard
  cases that are too narrow for the general smoke matrix. It generates the
  contains.any synthetic guard corpus and text/base64 file-backed mutation
  payloads, runs Go and C over the same fixtures, validates counter parity and
  benchmark schema, gates C steady-state file-backed mutation throughput with
  `LQL_BENCH_LOCKD_FILE_MAX_NS_PER_BYTE`, and gates C contains.any against
  explicit OR with `LQL_BENCH_LOCKD_CONTAINS_ANY_MAX_NS_RATIO` and
  `LQL_BENCH_LOCKD_CONTAINS_ANY_MAX_NS_DELTA` (default `2.00`). The ratio
  catches meaningful regressions while the absolute delta keeps single-run
  low-single-digit nanosecond per-byte variance from dominating otherwise
  low-latency rows. These are C-native guardrails; they do not define Go
  throughput as the C target.
  `make bench-check` invokes this target.
- `make bench-memory-check` runs a separate scalable Go/C/Lua streaming memory
  profile over a generated NDJSON fixture. By default it generates at least
  16 MiB of input using bounded per-record padding, runs
  `decision_only_selector`, `reuse_selector`, `reparse_selector_each_run`,
  `decision_only_source_selector`, `plus_value_selector`,
  `plus_value_source_selector`, and `plus_value_openjson_selector` over equality
  large-blob `contains`/`icontains`, and temporal fuzz replay selector cases
  including date-only equality, shorthand timestamp range, explicit range,
  explicit date-window, and `since=yesterday` date macro selectors, compares C
  and Lua counters against the Go oracle, validates supported C peak RSS against
  `LQL_BENCH_MAX_C_PEAK_RSS_BYTES`, and validates supported Lua peak RSS
  against `LQL_BENCH_MAX_LUA_PEAK_RSS_BYTES` or the C ceiling when unset. The
  profile forbids unsupported records and requires host process RSS timing for
  Lua through GNU
  `/usr/bin/time -f/-o` or Darwin `/usr/bin/time -l`. It is configurable with
  `LQL_BENCH_MEMORY_COUNT`, `LQL_BENCH_MEMORY_BLOB_BYTES`, and
  `LQL_BENCH_MEMORY_MIN_BYTES`, so the same gate shape can be scaled without
  changing the runner or weakening the normal
  smoke gate. The target builds and runs the optimized C benchmark helper for
  C performance rows while preserving debug/test builds for normal unit
  coverage. The same target also runs a separate Go/C-only mutation memory
  profile over the same generated fixture using `mutate_file_selector`,
  `mutate_file_plan`, and `mutate_source_selector`. Lua is deliberately
  excluded from this large mutation memory profile until the public Lua facade
  has a streaming mutation output sink; the gate still compares C mutation
  candidate/match counters against the Go oracle and applies the C RSS and
  steady-state time ceilings. It also runs a Go/C-only projection memory
  profile with `project_file_selector` and `project_source_selector`, projecting
  `/id` from matched candidates while large `/blob` fields remain unselected.
  That profile gates seekable and callback-source payload projection RSS/time
  without materializing whole candidates.
- `make bench-large-json-check` is the explicit 100 MiB streaming profile. It uses the
  generated large NDJSON fixture and a focused C/Lua streaming profile covering
  decision-only file-backed selection, seekable file-backed plus-value
  selection, and callback-source plus-value selection. The gate validates exact
  generated candidate/match/payload counts without
  asking Go to rescan the 100 MiB corpus, then applies the common RSS/time
  validator. Go remains the oracle for the broader medium-size parity benchmark
  gates where exhaustive mode coverage is practical. The 100 MiB gate writes
  `build/bench-large-json-check.jsonl` and is configurable with `LQL_BENCH_LARGE_JSON_COUNT`,
  `LQL_BENCH_LARGE_JSON_BLOB_BYTES`, `LQL_BENCH_LARGE_JSON_MIN_BYTES`, and
  `LQL_BENCH_LARGE_JSON_LOG` for local reproduction or reduced-size wiring checks.
  It is part of `make prerelease-hardening` and the normal `make release` gate.
- `make benchmarks-c` exercises the public liblql API for decision-only output
  and matched-only seekable plus-value payload access over a shared generated
  fixture.
- `make benchmarks-go` exercises `parity/cmd/lqlbench`, which uses the pinned
  Go module and emits stable JSON Lines without scraping `go test` output.
  Go helper records report `ns_per_op`; `steady_state` performs one untimed
  warmup fixture pass before the measured pass. The schema validator requires
  supported Go records to report timing.
- the current executable dataset matrix covers NDJSON and single-root JSON
  object fixture shapes for both library-style and CLI-style
  record forms, plus a lockd-shaped NDJSON fixture with session, tab, event,
  operation, timestamp, and payload fields, a mixed-root NDJSON fixture that
  interleaves scalar and object candidates, and the Go realworld benchmark
  fixture family: compact realworld NDJSON and pretty nested realworld streams
  with sparse events, dense components, nested hash fields, session ID arrays,
  metadata, timestamps, and nested payload blobs. All fixtures are generated
  once and shared by Go, C, and Lua.
- the current executable selector matrix covers equality, contains,
  `contains.any`, case-insensitive contains, timestamp comparison, date
  window, numeric range terms, and concrete numeric object-key/array-index path
  traversal over record-stream fixtures, lockd-style `/event="session_sync"`,
  `/event="tabs_update"`, and `/lockd/key` existence terms, plus nested
  `/records[]/...` selection over the single-root JSON fixture. Mixed-root
  selectors include an object-root pruning case and a one-record low-match case
  so capture-policy modes cannot be validated only on dense all-object streams.
  The full runner also mirrors Go realworld benchmark selectors for sparse and
  dense equality, no-match equality, numeric ranges, nested hashes, array
  membership, recursive field lookup, contains/icontains, any-value string
  terms, and multi-clause AND evaluation. The smoke gate includes compact
  multi-clause realworld selection and pretty/nested recursive hash selection
  so this fixture family remains part of the ordinary benchmark gate.
- the current executable mode matrix covers `decision_only_selector`,
  `decision_only_plan`, `reuse_selector`, `reparse_selector_each_run`,
  `decision_only_source_selector`, `plus_value_selector`, `plus_value_plan`,
  `plus_value_source_selector`, `plus_value_openjson_selector`,
  `plus_value_openjson_plan`, `mutate_file_selector`, `mutate_file_plan`, and
  `mutate_source_selector`; the scalable memory gate additionally covers
  `project_file_selector` and `project_source_selector`, and the lockd
  performance gate covers `mutate_file_backed_text` and
  `mutate_file_backed_base64`.
  Plus-value records assert equivalent payload counts and payload byte totals,
  while C exposes `seekable_range` payloads for seekable fixture files,
  including current open-read benchmark modes, without retaining candidate JSON
  after callback scope. Spool payloads remain reserved for non-seekable
  callback-source open-read modes.
  Mutation benchmark records assert equivalent candidate and match counts while
  timing compact matches-only mutation to a discard sink on C and Go. Numeric
  path selector cases mutate `/voucher/lines/10/bench`, so the mutation
  benchmark also exercises numeric object-key and array-index path writes.
  Lockd selector cases mutate `/processed=true`, matching the Go lockd fixture
  benchmark's observable mutation shape. Realworld selector cases map to the
  same mutation families as the Go realworld mutation benchmark: dense and
  sparse top-level sets, nested hash set, numeric increment, payload removal,
  nested metadata creation, and multi-mutation. Lua currently returns mutated
  output as a Lua string through its public facade, so Lua mutation is included
  in the smoke parity matrix but not in the scalable memory profile until the
  Lua facade exposes a streaming mutation output sink.
  Lockd file-backed mutation perf modes parse explicit `textfile:` and
  `base64file:` mutation values with file-value options enabled, stream the
  generated input through public mutation APIs, and count bytes per iteration
  as input JSON plus file-backed payload bytes.
  The current C plan benchmark reuses the parsed public `lql_selector` handle;
  it is a plan-shaped steady-state path, not a distinct compiled-plan API.
  The C native helper and Lua facade runner report `ns_per_op`; the schema
  validator requires timing for all supported Go, C, and Lua records. The C
  native helper and Go helper report OS `getrusage` peak RSS as
  `peak_rss_bytes`; the Lua facade runner records process peak RSS through the
  host `time` command when available. The memory profile requires positive
  `peak_rss_bytes` for supported Lua records.
- the current executable CLI-style selector matrix covers grouped
  equality/range, service contains, service case-insensitive contains, service
  `contains.any`, service `icontains.any`, and nested `/records[]/...`
  equivalents over the CLI-style single-root JSON fixture.
- `make benchmarks-lua` loads `lua/lql.lua`, which uses the direct Lua 5.5
  `lql.core` C module over public liblql APIs, creates a receiver-backed
  client with `lql.new()`, reuses parsed selector userdata for
  `reuse_selector`, reparses expression strings for
  `reparse_selector_each_run`, performs an untimed warmup pass for
  `steady_state`,
  proves plus-value payload readability through `match.write_json(callback)`
  while counting the original `match.size` byte contract rather than emitted
  normalized JSON bytes, and emits timed stable JSON Lines records. Lua
  `mutate_file` uses liblql's native seekable candidate mutation receiver path
  rather than re-querying and mutating with the same `FILE *`; this keeps the
  parser stream position stable on larger fixtures.
- `make benchmarks-parity` requires Go, C, and Lua benchmark implementations
  and fails on missing runners, strict-validation unsupported records, or
  counter divergence.

## Source Benchmark To Mirror

Use the existing Go benchmark shapes as the semantic and dataset baseline:

- library benchmark: `BenchmarkQueryStreamSynthetic`;
- CLI benchmark: `BenchmarkLQLSelectionBaseline`;
- mode helper: `runBenchmarkModes`, with `warmup_included` and `steady_state`.

The benchmark must not shell out to `go test` and scrape arbitrary
human benchmark output as its primary data model. It may invoke Go benchmark
code, but the shared comparison harness should emit a stable machine-readable
result record.

The benchmark must not normalize expectations around Go throughput. Go timing
is recorded so C and Lua regressions have a familiar reference, but public
liblql timing must be judged against C-native baselines or speedup floors once
a surface is claimed complete. A C implementation that repeatedly lands near
Go throughput on in-process library paths should be treated as an
implementation problem to investigate, not as a successful parity result.

## Dataset Matrix

Generate the same deterministic datasets for all implementations:

1. `large_ndjson`
   - repeated JSON objects separated by newlines;
   - root arrays are not valid NDJSON candidate streams and must not appear in
     parity or benchmark fixtures;
   - default count should mirror Go library benchmark scale:
     `LQL_BENCH_QUERY_NDJSON_COUNT`, fallback `20000`;
   - CLI-style benchmark may also support `LQL_BENCH_NDJSON_COUNT`, fallback
     `30000`.

2. `large_single_json`
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
can query a 100 MiB JSON input while constrained to a 128 MB process memory
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

The realworld benchmark family mirrors the synthetic corpus used by the Go
`BenchmarkQueryStreamRealworld` and `BenchmarkMutateStreamRealworld` tests.
It includes both compact NDJSON and pretty multi-line JSON values. Records have
top-level `event`, `component`, `code`, `active_idx`, and `tab_count` fields;
a nested `query.hash`; optional `session_ids` arrays; `timestamp` and `meta`
objects; and either object-shaped or deeply nested array-shaped `payload`
values. Sparse target values appear at deterministic intervals so selectors
exercise both dense and low-match scans.

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

For realworld query parity, also include:

- `/event="session_sync"`
- `/component="edge"`
- `/event="__nope__"`
- `/code>=11`
- `/query/hash="c5d2460186f7233c927e7db2dcc703c0a3a8e0d5f0d8a3c5b4f1e2d3c4b5a697"`
- `/session_ids[]="sid-0a3f-target"`
- `/.../event="session_sync"`
- `/.../hash="c5d2460186f7233c927e7db2dcc703c0a3a8e0d5f0d8a3c5b4f1e2d3c4b5a697"`
- `contains{field=/event,value=sync}`
- `icontains{field=/component,value=EDGE}`
- `contains{field=/event,any=sync|__nope__}`
- `icontains{field=/component,any=EDGE|__nope__}`
- `/component="edge",/event="session_sync",/active_idx=0,/tab_count=1,exists{/session_ids},/code>=10`

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

5. `decision_only_source_selector`
   - read fixture bytes through callback-source or ordinary reader APIs instead
     of passing a seekable file handle as the query source;
   - run query in decision-only mode;
   - count candidates and matches;
   - do not expose payloads or capture candidate values.

6. `plus_value_source_selector`
   - read fixture bytes through callback-source or ordinary reader APIs instead
     of passing a seekable file handle as the query source;
   - include callback-scoped payload access;
   - for C, the expected payload source type is `spooled` because the public
     source API is non-seekable and cannot reconstruct payloads by offset;
   - do not retain complete candidate payloads after the match callback
     returns.

7. `plus_value_openjson_selector`
   - force a very small memory threshold for `large_single_json`;
   - read each matched/callback payload through the callback-scoped open/read
     source path, preferring seek/reread by candidate offset for seekable
     inputs and using spooled handles only for non-seekable inputs;
   - verify byte counts and cleanup behavior.

8. `plus_value_openjson_plan`
   - plan variant of seekable-range or spool/open-read mode.

Also mirror CLI benchmark parse behavior:

9. `reuse_selector`
   - parse/compile selector once before timing loop.

10. `reparse_selector_each_run`
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
- OS-reported peak resident set size in bytes when available;
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
  "payload_source_type": "none",
  "fixture_sha256": "0000000000000000000000000000000000000000000000000000000000000000",
  "ns_per_op": 1234567,
  "peak_rss_bytes": 3145728,
  "allocs_per_op": null,
  "unsupported": false,
  "unsupported_reason": ""
}
```

The comparison script may also print a human summary table, but JSON Lines is
the contract for tools and regression gates.

Known `payload_source_type` spellings are `none`, `seekable_range`, `spooled`,
`callback_payload`, and `lua_liblql`. New spellings require a validator update
and fixture coverage so buffered/materialized behavior cannot be hidden behind
ad hoc labels.

## Lua Requirements

The Lua implementation lives in this repository and must be benchmarked through
the repository's Lua facade, not by shelling out to Go or `clql` and not by
calling C private test helpers directly. The Lua benchmark runner must go
through `lua/lql.lua`, which loads the direct Lua 5.5 `lql.core` C module over
public liblql APIs and creates a real `lql *` receiver-backed client.

Lua benchmark entry points should support:

- loading a selector;
- optional reusable compiled selector/plan if the Lua API exposes one;
- streaming candidates from a dataset file;
- returning candidate and match counts;
- callback-scoped payload/open-read modes when Lua exposes payload handles;
- callback-scoped payload write callbacks that avoid materializing the whole
  payload as a Lua string.

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

Parity benchmark gates fail on behavioral divergence, benchmark runner errors,
unsupported records in claimed gate profiles, bounded-memory violations, and a
loose C-native steady-state throughput ceiling for supported C records. The
throughput ceiling is intentionally expressed as C `ns_per_op/bytes_per_iter`;
Go throughput is not a target for C.

Performance thresholds should be added after:

- C and Lua have feature parity for the relevant selector classes;
- baseline logs exist for the local lifecycle environment and release target;
- the measured path is not dominated by process startup, shell orchestration,
  temporary CLI facades, or disk I/O;
- thresholds are expressed as C-native ratios or guarded baselines with enough
  tolerance to avoid noisy failures.

Committed baseline logs live under `bench/baselines/` and are refreshed with
`make bench-freeze-baseline`. The freeze target waits for a quiet host load,
runs the smoke, lockd, scalable memory, and 100 MiB benchmark profiles, validates
the emitted JSON Lines records, and updates the baseline checksum manifest.

Suggested staged gates:

- Go/C/Lua behavior must match for supported selector classes;
- C library steady-state memory must remain bounded for streaming modes and
  must not grow with total input size, candidate size, match count, or result
  set size;
- C library steady-state records are gated by
  `LQL_BENCH_MAX_C_STEADY_STATE_NS_PER_BYTE`, defaulting to a conservative
  C-native ceiling of `500` ns/byte. This leaves substantial headroom over
  current source-spooling-heavy smoke rows while still failing severe
  regressions without treating Go as the performance baseline;
- C library plus-value/open-read modes must prove callback-scoped payload
  access without candidate retention and must have a documented C-native
  baseline distinct from CLI-mediated `clql` timing;
- C lockd file-backed mutation modes are gated through
  `make bench-lockd-perf-check`, with text and base64 payload throughput
  thresholds expressed in C `ns_per_op/bytes_per_iter`;
- C contains.any specialization is gated against the corresponding explicit OR
  selector through `LQL_BENCH_LOCKD_CONTAINS_ANY_MAX_NS_RATIO` and
  `LQL_BENCH_LOCKD_CONTAINS_ANY_MAX_NS_DELTA`; this is a C-native regression
  guard for the selector engine and is separate from Go timing;
- Lua memory is gated for the current direct-module seekable-file and
  callback-source benchmark workflows. Lua-supported modes must keep bounded
  memory and must not materialize complete candidates or result sets to satisfy
  the benchmark.

Any performance gate must print actionable diagnostics with dataset, selector,
mode, observed value, baseline/threshold, and reproduction command.

## Non-goals

- Do not benchmark different generated datasets per language.
- Do not treat Go throughput as the desired C performance level.
- Do not compare pretty/colorized JSON output. `clql` intentionally does not
  reimplement `prettyx` colorized output.
- Do not make benchmark success depend on exact allocation equivalence across
  languages.
- Do not hide unsupported selector classes by filtering them out.
- Do not call a benchmark streaming if it materializes the full dataset first.
- Do not satisfy payload modes by retaining complete candidate copies outside
  the callback lifetime.
- Do not pass the 100 MiB streaming profile by increasing memory limits, using
  temporary files as an undisclosed full-input staging substitute, or disabling
  payload modes that the profile requires.
- Do not treat the 100 MiB streaming profile as permission for memory growth
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
- C library performance gates can distinguish behavioral correctness from a
  C-native performance regression;
- `make benchmarks-parity` fails if any required implementation is missing;
- `make bench-check` runs a small deterministic matrix suitable for local
  confidence.
- `make bench-large-json-check` runs the explicit 100 MiB memory profile with
  seekable file-backed and callback-source plus-value coverage; reduced-size
  overrides are available only for checking target wiring.
- `make bench-freeze-baseline` refreshes the committed baseline logs only after
  the host has stayed below the configured quiet-load threshold.
