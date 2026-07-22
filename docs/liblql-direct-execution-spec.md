# Superseded: LoneJSON-Backed Direct Execution Specification

> **Archived design record — do not implement from this document.** Its
> LoneJSON runtime dependency, single-entry-point contract, and spool rules are
> rejected. The governing v0 architecture is
> [`liblql-self-contained-execution-spec.md`](liblql-self-contained-execution-spec.md),
> and the installed header is the API authority.

## Historical liblql Direct Execution Rewrite Specification

## Historical Authority And Status

At the time of writing, this superseded every earlier Candidate Run,
candidate-transform, hybrid, port, parity, and benchmark document. It no
longer governs the repository.

The repository is deliberately at a deletion checkpoint. Only the allocator,
selector AST/parser, temporal support, vendored LoneJSON basics, public header
smokes, and the Go benchmark oracle remain. There is no query, projection,
mutation, CLI, Lua binding, C parity adapter, C benchmark runner, or release
surface. This is intentional. Do not restore deleted code as a shortcut.

The next implementation session starts from this state. It must implement the
architecture described here in coherent slabs, not retain compatibility code
while iterating.

## Deletion Proof

`scripts/check_direct_execution_reset.sh` is the repository-wide deletion
gate. Outside this specification, the handover, that check, and the preserved
`reference/` oracle assets, it rejects Candidate Run/candidate-transform names,
old execution methods, old public stream methods, and evaluator-only AST state.
It also asserts that the old executor, CLI, benchmark, Lua, and parity
directories are not tracked.

The reference assets preserve the Go oracle, fixtures, and Go/C matrix, not
executable C implementation. The legacy harness must never be compiled or
called as the new C runner. Every new implementation commit keeps the deletion
gate passing.

## Reference Behavior

The behavior authority is `pkt.systems/lql v0.17.1`:

- module: `pkt.systems/lql`;
- tag commit: `273f918463c5f9e85dbbed7f0d0c1d4a79e32a11`;
- module checksum: `h1:eXQ4Hv7FScVh9wQHIyJYzplJjEZ0SnZaI7kgwlLXJOU=`.

Go controls observable LQL semantics. C is free to use a different internal
design only when every accepted observable result is identical and the C
resource/performance rules below are met.

The only intentional parser/framing exclusions are:

1. A root JSON array is a hard error at every liblql and clql NDJSON stream
   entry point. It is never flattened, recursively or otherwise. Root arrays
   must not appear in Go/C parity or performance fixtures. This intentionally
   diverges from Go lql, whose JSON stream APIs flatten array documents; liblql
   preserves NDJSON framing instead.
2. LoneJSON remains the strict JSON parser. Go decoder acceptance of malformed
   surrogate sequences or adjacent leading-zero numbers is not required.

No other selector, temporal, projection, mutation, ordering, result, or error
gap is permitted without adding a named exception here and obtaining an
explicit decision.

## Non-Negotiable Architecture

liblql owns all LQL execution. During this rewrite, the vendored LoneJSON
source is an iteration harness and is used only through public `lonejson.h`;
liblql must not use LoneJSON private headers or symbols.

The v0 deliverable does **not** vendor or embed a private LoneJSON instance.
It links the supported upstream LoneJSON binary ABI (`.a` for static linkage
or `.so` for shared linkage), so one LoneJSON instance can be shared by the
other downstream components in a deliverable. The current source-vendored
harness must therefore preserve that boundary: no private APIs, no
cross-library IPO/LTO assumptions, and no optimization whose validity depends
on compiling LoneJSON and liblql as one translation unit or with one compiler.
Changes made to vendored LoneJSON must be general public-API improvements
suitable for upstreaming.

## Compiler And Performance Baseline

liblql and its LoneJSON integration remain ANSI C89 compatible. CMake has no
`C_STANDARD 89` value, so GCC builds explicitly end with `-std=c89`; this is
not permission to require C99 or a newer language standard.

GNU GCC is the authoritative C compiler for the performance acceptance gate.
Every required C/Go row must reach at least `1.0x` when liblql and the
rewrite-time LoneJSON harness are separately compiled with GCC at the normal
release optimization level, without cross-library IPO/LTO. This models the
upstream binary-ABI deployment boundary. The exact GCC release and benchmark
host are recorded with each benchmark run. Do not pin the project preset to a
non-GCC compiler as a performance workaround.

LoneJSON provides JSON input, strict framing, token/value events, diagnostics,
escaping, JSON writing, and ordinary bounded spooling primitives. It must not
contain selector-aware options, LQL mutation actions, output decisions, a
transform program, Candidate Run, or candidate-transform compatibility code.

When liblql needs to preserve a current record while its selector decision is
not known until the record closes, LoneJSON may provide a public,
library-neutral event-fed rewriter over its existing normalized-path rewrite
options. The rewriter accepts ordinary `lonejson_value_visitor` events and
emits one compact transformed JSON value through a caller-owned sink. It owns
only generic JSON framing, normalized path matching, replacement emission,
decoded key buffering, escaping, and writer error propagation. It must not
accept a selector, an LQL mutation expression, a record decision, or any
LQL-specific option. Its public state and init/close/cleanup rules must be
usable across the upstream static and shared ABI boundary.

liblql compiles its mutation action into those generic rewrite options and
composes the rewriter with direct selector state to build at most one
transformed current-record spool. It decides whether to emit or discard that
spool only after the selector result is final. This replaces
capture-then-reparse mutation execution; it is not permission to restore an
input/result cache or a selector-aware transform layer in LoneJSON.

This document describes the direct-execution reset baseline. The active
self-contained cutover is specified in
`docs/liblql-self-contained-execution-spec.md`: liblql owns JSON scanning and
emission for direct execution and selector AST JSON, without a LoneJSON runtime
or link dependency in the target architecture.

liblql compiles selector, projection, and mutation input into immutable flat
programs. A single direct stream executor consumes JSON events and owns, for one
current record only:

- path and container state;
- selector truth and temporal comparison state;
- projection inclusion state;
- mutation ordering and writer state;
- output framing, counters, limits, and error precedence.

The executor must not be a callback adapter around a generic transform engine.
In particular, do not move Candidate Run, action staging, old-value replay,
deferred transform decisions, or a generic candidate event protocol into
liblql under a different name.

The existing selector AST is language data only. Its removed evaluator caches,
observer families, predicate flattening, hit indexes, and path execution
caches must not be reintroduced as the old evaluator. The new compiled program
is derived afresh from the AST and is owned by the new executor.

## Historical Public Receiver Contract (Rejected)

The rejected public surface was receiver-based and had one stream engine. The
exact declarations now belong in `include/lql/lql.h`; the historical contract
below is retained only to explain the abandoned design:

- `lql_stream_apply(lql *, const lql_stream_request *,
  lql_stream_result *, lql_error *)` was intended as the only execution entry
  point;
- a request accepts one reader callback, optional writer callback, compiled
  selector/projection/mutation handles, explicit output mode, matched-only
  policy, limits, an optional synchronous cancellation predicate, an optional
  C `time_t` source for relative dates, and optional decision/value callbacks;
- file and buffer helpers, when added, adapt into that request and invoke the
  same executor. They must not become separate file/source execution paths;
- output modes cover decision-only, selected record output, projection,
  mutation, and projection-before-mutation;
- value callbacks expose only a completed callback-scoped public payload when
  that contract is requested. They do not expose parser events or transform
  instructions;
- results report records seen, records matched, bytes consumed, early-stop
  state, and stop reason. Invalid arguments zero the result;
- selector, projection, and mutation parse/build/destroy methods remain
  receiver-owned handles. There is no compatibility requirement for removed
  old method names or structs.

`clql` is a consumer of this receiver contract. It is not a receiver shell and
does not define the executor architecture. Reintroduce it only after the
receiver stream contract is tested; it must be a thin argument/parser and I/O
adapter over `lql_stream_apply`.

Lua is also a later consumer. It must not carry a second executor or call a
CLI subprocess.

## Required Semantics

### Framing And Streaming

- Inputs are strict NDJSON: repeated top-level JSON values, excluding root
  arrays. Objects and scalar values are parsed as individual records; a
  non-empty selector does not match a scalar that lacks the selected path.
- A completed earlier record remains observable when later input is malformed;
  counters and output reflect work completed before the failure.
- Duplicate object keys are visited in source order.
- Every emitted record is compact JSON followed by one newline. No input,
  record list, match list, or result list may be materialized as a whole.
- Match, record, and byte limits stop promptly with Go-equivalent precedence.
  Callback-requested graceful stop is distinct from callback failure.

### Selectors And Temporal Rules

Implement the accepted Go selector grammar and AST behavior: AND/OR/NOT,
equality and inequality, contains/icontains, prefix/iprefix, range, date, in,
exists, JSON Pointer decoding, numeric object-key versus array-index handling,
object wildcard, array wildcard, any-child wildcard, recursive descent,
shorthand, brace forms, aliases, quoted values, and indexed composition.
The string predicate operators are textual: scalar-looking `value` and `any`
needles are treated as text for `contains`, `icontains`, `prefix`, and
`iprefix`, and target JSON strings plus boolean/number scalar values are matched
through their textual value. JSON `null` is not treated as text. Typed JSON
scalar semantics are reserved for equality/inequality and `in` membership.

Date-only equality, naive UTC datetimes, timezone offsets, nanoseconds,
relative `since` macros, and range endpoints must match the Go reference. The
current-time macro is evaluated per execution, not cached with the selector.

### Projection And Mutation

Projection must match Go path normalization, duplicate removal, parent/child
conflict errors, object/array construction, missing-field behavior, escaped
pointers, and compact output.

Mutation must match Go parsing and execution for set, remove, increment,
creation of missing object paths where Go permits it, typed/quoted values,
time normalization, brace shorthand, comma/newline-separated top-level
mutation clauses, explicit file/text/base64 values, concrete numeric paths,
object/array wildcards, recursive paths, and error precedence.

File-backed mutation values are disabled by the default mutation parser and
must be enabled with explicit parse options. `file:` auto-selects text or
base64 by inspecting the source file, `textfile:` requires valid UTF-8 text
without NUL bytes, and `base64file:` streams a base64 JSON string. The parser
stores a local resolved path when using the default backend; file bytes are
read by the mutation emitter so CLI consumers do not materialize or rewrite
the value. The default backend opens a fresh local file for every inspection
or output pass. A caller may instead supply paired fresh-open/read/close
callbacks. That source contract is deliberately non-seekable: each open starts
at byte zero, and liblql never retains caller `FILE *` values, asks a source to
rewind, or buffers a complete file. The callback context is borrowed by the
mutation handle until destruction.

`time:...=NOW` accepts an optional parse-option C `time_t` source, and stream
requests accept the same source for selector-relative date terms. The default
is `time(NULL)`. Selector datetime literals intentionally retain Go lql's
naive-UTC datetime forms. `time:` mutation values intentionally do not: they
accept `NOW` or strict timezone-bearing RFC3339/RFC3339Nano timestamps and
normalize those timestamps to UTC. liblql deliberately rejects Go `time.Parse`
leniencies such as comma fractional seconds or out-of-range timezone
components. Accepting process-local date names would change the LQL language and
is intentionally out of scope.

For a combined operation, selection occurs on the original record, projection
is applied before mutation, mutation is applied only when selection and output
policy require it, and unmatched-record preservation follows the explicit
matched-only setting. This ordering is a behavior gate, not an optimization.

## Streaming And Memory Invariants

Streaming means direct producer-to-consumer flow. The implementation must not
hide full-record, full-input, or all-result materialization behind a streaming
API.

An optional request cancellation predicate is synchronous and creates no
threads. It is checked before reader refills and before completed-record
dispatch. A non-zero result ends successfully with
`LQL_STREAM_STOP_CANCELLED`; the maximum check latency is one bounded reader
buffer plus the current callback-free parser work.

- Default selection, projection, and mutation paths use no candidate/result
  cache and no internal capture/replay path.
- A bounded current-record spool is allowed only for an observable public
  payload callback or a semantic delayed-output requirement. It is reset and
  released before processing the next record.
- Compiled selector/projection/mutation programs and bounded parser/writer
  stacks are allowed. They are not input/result caches.
- Live heap is the primary embedded-memory invariant. Peak direct-executor
  heap must stay at or below 256 KiB and remain independent of total input
  bytes, record count, match count, result count, and repeated executions in
  one process. Every `lql_new` receiver also has an independent 8 MiB
  allocation budget shared by its parsed selector, projection, and mutation
  handles plus its temporary execution plans and compatibility spools. The
  spooled compatibility executor spills the current record after its bounded
  in-memory spool. It may scale only with program size, nesting depth, and
  bounded transport buffers.
- The live-heap gate covers selection, projection, mutation, sparse matching,
  a 100 MiB JSON record, and repeated large records. It must prove that no
  temporary full-record allocation or retained spool grows across executions.
- RSS is a diagnostic deployment warning, not the primary release invariant:
  fresh-process RSS above 8 MiB requires investigation, but platform loader,
  libc, code-page, and TLS residency do not by themselves fail the memory
  contract.

## Performance Contract

C must beat Go on every accepted Go/C benchmark row. This is a hard release
gate:

```text
Go steady-state ns/op / C steady-state ns/op >= 1.0
```

There are no tolerated rows below `1.0x`. `1.2x` is the stretch target after
the hard floor passes; do not add obscurity, caching, or architecture debt to
chase it.

The accepted matrix includes decision-only, selected-output, callback-source,
projection, mutation, projection-before-mutation, nested/recursive selectors,
temporal and numeric selectors, sparse and dense matches, realworld-shaped
records, lockd-shaped records, the 100 MiB large JSON case, and repeated large
records. Each row uses the same fixture bytes, selector, operation, output
policy, and counters in Go and C. Root arrays are excluded.

Compile reusable programs outside the timed loop. Do not warm, cache, or reuse
input records, result sets, selected payloads, or mutation outputs. Record both
`warmup_included` and `steady_state` rows; the hard speed gate applies to every
accepted steady-state row and any accepted warmup row explicitly marked as a
gate.

## Oracle And Benchmark Assets

`reference/go-benchmark/` is committed source, not a historical baseline:

- `cmd/lqlbench` runs the pinned Go reference and emits benchmark records;
- `cmd/benchvalidate` validates row shape/counter pairing and supports
  `--min-c-go-speedup=1.0`;
- `go.mod` and `go.sum` pin the reference module.

`reference/benchmark-harness/legacy-run-parity-benchmarks.sh` preserves the
exact pre-reset fixture generator and matrix. It defines the `large_ndjson`,
single-root, selection, lockd, realworld compact, realworld pretty/nested, and
lockd file-value data sets; selector rows for equality, range, contains,
icontains, any alternatives, numeric paths, recursive paths, and sparse/dense
realworld cases; and the following operation modes:

```text
decision_only_selector       decision_only_plan
reuse_selector               reparse_selector_each_run
decision_only_source_selector
plus_value_selector          plus_value_plan
plus_value_source_selector   plus_value_openjson_selector
plus_value_openjson_plan
project_file_selector        project_source_selector
mutate_file_selector         mutate_file_plan
mutate_source_selector
mutate_file_backed_text      mutate_file_backed_base64
```

The reference harness also specifies deterministic fixture generation, root
array rejection, fixture SHA-256, count/payload-byte comparison, Go/C order
alternation, CPU pinning where available, and both `warmup_included` and
`steady_state` rows. It is not runnable as-is because its old C and Lua calls
were deliberately removed. The new harness preserves these definitions and
replaces only those calls with the new direct C runner. It must require `go`
and `c` rows, reject missing pairs, and run the Go/C counter comparison before
timing validation.

The new C benchmark runner must emit the same JSONL schema and every Go/C row
must have equal fixture SHA-256, record count, match count, payload count, and
payload bytes before timing is compared. Behavioral output equality is proved
by the C/Go parity corpus separately; a benchmark row is never accepted merely
because its counters agree.

Do not restore old C benchmark code, CGo bridge code, benchmark baselines, or
old performance claims. Write a new C runner over the direct public receiver.

## Verification Order

Implement in large coherent slices and only run broad gates at meaningful
boundaries:

1. Add the direct stream request/result API and strict-NDJSON rejection tests.
2. Implement direct selector execution and Go selector/temporal parity.
3. Add direct output, projection, and mutation execution with behavior tests.
4. Add the thin `clql` consumer and CLI parity tests.
5. Add the new C benchmark runner and pair it with the preserved Go oracle.
6. Run complete C tests, Go parity, sanitizers, fuzzing, multi-large-record
   RSS gates, and the full Go/C performance matrix.

Do not test each small internal edit. Do test each completed behavioral slab.
No task is complete until all required gates pass and every accepted C row is
at least `1.0x` Go.

## Prohibited Shortcuts

- restoring or adapting deleted Candidate Run/candidate-transform code;
- using LoneJSON internal headers, private symbols, or an optimization that
  assumes a co-compiled LoneJSON implementation during this rewrite;
- root-array flattening in liblql, clql, parity, or benchmarks;
- result/candidate caches or hidden capture/replay for speed;
- whole-input or whole-record materialization disguised as streaming;
- a separate CLI/Lua execution path;
- declaring completion after deletion, compilation, a partial matrix, or a
  benchmark median. Every required row and RSS invariant is mandatory.
