# liblql Port Specification

## Goal

Port `pkt.systems/lql v0.17.1` from Go to C89 in this repository as
`liblql`, with a companion CLI named `clql`.

The C port must be suitable for lockd-style local JSON search and mutation
workloads. It must preserve observable LQL behavior from the Go implementation
where claimed, prove the C product contract through native C tests, use Go as
the semantic oracle for convergence, and use `lonejson` for JSON parsing,
serialization, validation, stream framing, escaping, payload capture, and
rewrite support.

The implementation must not reference the adjacent Go source checkout from
repository files. Go oracle checks must go through the `parity/` Go module,
which pins `pkt.systems/lql v0.17.1`. Repository files must not derive
behavior from, or reference, an adjacent Go source checkout.

## Release Artifacts

The repository produces two artifact families:

1. `liblql`
   - pkt.systems lifecycle-compatible C SDK tarballs;
   - static and shared C libraries where supported;
   - public headers under `include/lql/`;
   - CMake package config and pkg-config metadata;
   - license, README, examples, and dependency provenance.

2. `clql`
   - CLI tarballs named `clql-<VERSION>-<TARGET>.tar.gz`;
   - equivalent to the Go `lql` binary for supported behavior;
   - intentionally does not reimplement `pkt.systems/prettyx` colorized JSON.

Target matrix follows the pkt.systems lifecycle:

- `x86_64-linux-gnu`
- `x86_64-linux-musl`
- `aarch64-linux-gnu`
- `aarch64-linux-musl`
- `armhf-linux-gnu`
- `armhf-linux-musl`
- `arm64-apple-darwin` when the Darwin toolchain is available

## Dependency Boundary

`lonejson v0.35.0` or newer is the JSON substrate. liblql obtains lonejson
from the official GitHub release SDK archives for each target and verifies the
archive SHA-256 before installing it into the local dependency cache.

liblql must not implement bespoke JSON parsing, tokenization, escaping,
serialization, stream framing, compacting, or payload spooling when lonejson can
own that behavior.

If a required JSON capability is missing from the available lonejson release,
liblql must treat that as an external dependency capability gap. It must not
hide an alternate JSON implementation inside liblql.

## Public C API Intent

The public API is C89-compatible and installed under `include/lql/`.

The primary public C API is receiver-function based. Callers create an
instantiatable `lql *` with `lql_new()`, invoke operations as
`ctx->operation(ctx, ...)`, and release it with `ctx->destroy(ctx)`. Selector,
query, payload, projection, compact, mutation, and receiver cleanup operations
are receiver methods only; standalone public functions are limited to
construction, diagnostics, and version/capability helpers.
Project-owned code must also use receiver calls directly rather than recreating
removed operation free functions or free-operation cleanup aliases through local
macros or static wrapper shims.

The API should be handle-oriented and explicit about ownership:

- parser/compiled selector handles are owned by the caller and destroyed with
  receiver cleanup methods;
- result payload handles are valid only for documented callback lifetimes and
  must not imply retained candidate copies;
- error messages are actionable and available through explicit error objects.

All liblql-owned allocation must pass through the internal central liblql
allocator surface or receiver cleanup methods. Production code must not use
direct `malloc`, `calloc`, `realloc`, or `free` outside the allocator
implementation. Public APIs must avoid returning liblql-owned heap memory
unless they also provide an ownership-specific receiver cleanup method, so
downstream users do not cross allocator boundaries or depend on allocator
wrapper functions.

The API should eventually expose these surfaces:

- selector parse and receiver destroy;
- reusable selector plan/compiled state;
- selector evaluation over one arbitrary JSON value;
- streaming query over arbitrary candidate streams;
- projection path parse/plan and projection execution;
- mutation parse/plan and mutation execution;
- streaming mutation over arbitrary candidate streams;
- version and capability query helpers.

The API must name behavior precisely. Do not call an API streaming unless bytes
or records flow producer-to-consumer without full-message materialization.
In particular, streaming query APIs must not copy, concatenate, retain, or
materialize complete candidate payloads behind the caller's back.
The ideal is zero allocation during steady-state query execution. Where
allocation is unavoidable, it must be explicit, bounded, and attributable to
selector state, caller-provided buffers, small parser state, or documented
spooling handles rather than total input size.

## Selector Scope

Selectors must converge to Go `pkt.systems/lql v0.17.1` behavior.

Required selector features:

- explicit terms:
  - `eq`
  - `contains`
  - `icontains`
  - `prefix`
  - `iprefix`
  - `range`
  - `date`
  - `in`
  - `exists`
- logical composition:
  - `and`
  - `or`
  - `not`
- shorthand:
  - `/field="value"`
  - `/field!=value`
  - `/field>10`
  - `/field>=10`
  - `/field<10`
  - `/field<=10`
- JSON Pointer field paths;
- array indexes;
- wildcard path semantics:
  - `*` object child values only;
  - `[]` array elements only;
  - `**` object values or array elements one level down;
  - `...` recursive descent;
  - bracket sugar such as `/items[]/sku`;
- temporal semantics:
  - date-only values;
  - RFC3339 and RFC3339Nano;
  - naive UTC datetimes;
  - date equality intersection;
  - numeric and datetime range bounds;
  - `date.since` macros: `now`, `today`, `yesterday`.

Unsupported selector features must be explicit in tests and benchmark output
until implemented. Silent omission is not allowed.

## Projection Scope

`clql -f/--field` and the C API projection surface must match Go behavior.

Projection paths are JSON Pointer paths. Multiple fields should produce the
same observable JSON structure as Go LQL. Missing projection paths should be
handled according to Go parity, including whether an output is suppressed when
no requested field is found.

Projection must use lonejson path-aware visiting/capture where possible and
must not materialize full streams unless the API is explicitly named buffered.

## Mutation Scope

Mutations must converge to Go `pkt.systems/lql v0.17.1` behavior.

Required mutation features:

- set:
  - `/state/status=running`
- numeric increment/add:
  - `/state/retries++`
  - `/state/retries=+3`
- delete:
  - `rm:/state/legacy`
- time normalization:
  - `time:/state/updated=NOW`
- brace shorthand:
  - `/state{/owner="alice",/note="hi"}`
- wildcard path behavior matching selectors where Go supports it;
- streaming file-backed mutation values:
  - `file:`
  - `textfile:`
  - `base64file:`

File-backed mutation values are opt-in and must be explicit in the API and CLI.
They must stream through lonejson source/sink behavior rather than reading
entire files into hidden buffers.

## Streaming Query Scope

Streaming query is a core requirement.

The design target is that very large JSON inputs remain queryable on very small
machines. A 1 GB JSON input under a 128 MB process memory budget is the baseline
acceptance profile; a 1 TB JSON input on an 8 MB embedded machine is the
architectural stress model. These are not tuning goals: steady-state query
memory must be bounded by configured working buffers and parser/selector state,
not by total input size, candidate size, match count, or result set size. The
implementation must not require a complete candidate, complete stream, or
complete result set to reside in memory.

The stream API must handle:

- one top-level JSON value;
- NDJSON / repeated top-level JSON values;
- top-level arrays as streams of candidate values;
- large candidates with bounded memory;
- decision-only mode;
- plus-value mode with callback-scoped payload access;
- matched-only callbacks;
- seekable/rewindable sources where matched payload access is reconstructed
  from candidate offsets and byte sizes without capture;
- caller-managed or lonejson-managed payload sinks when supported, without
  liblql retaining complete candidate copies;
- non-seekable sources where plus-value output uses caller sinks or bounded
  spooled handles rather than memory capture;
- stop controls:
  - max matches;
  - max candidates;
  - max bytes read;
  - callback-requested graceful stop.

Candidate payload access must distinguish:

- no payload captured;
- seekable source range: candidate offset plus byte size, read only after a
  match decision;
- callback-scoped streaming/sink payload access;
- callback-scoped spooled payload handles;
- caller-managed payload sinks.

Streaming query must not use in-memory compact JSON capture as an
implementation shortcut. Any future API that deliberately materializes complete
candidate payloads must be named as buffered/materialized behavior, must be
opt-in, and must not be used to satisfy the streaming query requirement.
For very large matching values, plus-value behavior must expose a streaming
seekable range, sink/source, or spooled handle; it must not construct one
contiguous in-memory JSON value merely to return it to the caller. If the input
source is seekable or rewindable, liblql must prefer offset/size based reread
over candidate capture. Capture is only justified when the source cannot be
revisited or when the caller explicitly selects a capture mode.

As of lonejson `v0.35.0`, the installed public header confirms that
`lonejson_candidate_info` exposes candidate index, stream offset, byte size, and
payload size as `lonejson_uint64` range values. liblql should therefore treat
CR 3 plus CR 6 as sufficient for decision-only candidate streaming and
seekable-source payload reconstruction. The remaining work is liblql source
policy, not JSON parser work:

- classify public input sources as seekable/rewindable or non-seekable;
- expose seekable source ranges as callback-scoped payload handles;
- use lonejson `CAPTURE_NONE` for decision-only and seekable plus-value paths;
- use caller sinks or spooled handles only for non-seekable plus-value paths.

Seekable range APIs use 64-bit liblql offsets and sizes. When a platform
`FILE *` seek cannot represent a 64-bit range offset, liblql must fail the
operation rather than truncating or wrapping the requested offset.

## CLI Scope

`clql` should mirror the Go `lql` CLI where supported:

- selector arguments;
- `--or` / `-O`;
- `--field` / `-f`;
- `--mutate` / `-m`;
- `--inline` / `-i`;
- `--write` / `-w`;
- `--compact` / `-c`;
- `--matches-only` / `-M`;
- `--enable-file-mutations` / `-F`;
- `--help` / `-h`;
- `--version` / `-v`.

`--theme` / `-t` is accepted as a compatibility no-op. The Go CLI uses it only
to select a prettyx palette; the C CLI intentionally does not implement
colorized pretty output. Accepted theme spellings must not change compact JSON
output, selector behavior, projection behavior, or mutation behavior.

The CLI must produce actionable errors with stable wording where practical.

## Lua Scope

The Lua implementation is part of this repository's parity story.

Lua should expose the same high-value behavior as the C library:

- selector parse/evaluate;
- streaming query;
- projection;
- mutation;
- parity benchmark entry points.

The Lua implementation should use public liblql/lonejson surfaces rather than
duplicating LQL behavior independently unless an explicit Lua facade layer is
needed for DX.

Lua support targets Lua 5.5 only. The Lua facade must not shell out to `clql`;
it must load a direct C module that depends on the public liblql SDK surface.
The module must not use private liblql headers, private lonejson APIs, or
symbol interposition tricks, and it must be safe to load in a process that
already links liblql and other Lua bindings.

The Lua facade must expose a real liblql client object. `lql.new()` owns a
public `lql *` receiver created through `lql_new()`, and method calls dispatch
through that receiver. Lua must not emulate the client with a table of
module-level wrappers, and it must not depend on `clql`.

## Verification Requirements

Verification is the primary quality gate.

The verification strategy has three distinct layers:

- C-native product tests define what `liblql` and `clql` promise to downstream
  C, CLI, and Lua users. These tests assert public API ownership, callback
  lifetimes, out-parameter state, partial I/O, cleanup, diagnostics,
  bounded-memory semantics, and observable selector/projection/mutation
  behavior.
- Go oracle tests compare claimed LQL language and transformation behavior
  against `pkt.systems/lql v0.17.1` while the port is converging. They are
  convergence checks, not C unit tests.
- Benchmarks verify behavioral counters across implementations and, for mature
  public `liblql` paths, enforce C-native performance and memory expectations.
  Go timing is context, not the desired C performance level.

Required gates:

- C SDK contract tests for every public liblql behavior, with observable
  assertions for the C API's ownership, error, callback, streaming,
  bounded-memory, and result semantics;
- public header standalone compile tests;
- C89 consumer tests;
- CMake install-tree consumer tests;
- pkg-config consumer tests;
- `clql` smoke tests;
- Go library parity tests through `parity/` while the port is converging; these
  are oracle/reference checks for selector language and transformation
  behavior, not substitutes for C API contract tests and not a source to
  mechanically copy into C unit cases;
- Go CLI parity tests for `clql` versus the Go `lql` binary behavior where the
  C CLI claims compatibility;
- Lua parity tests when Lua facade exists;
- sanitizer tests;
- package verification and privacy/relocatability gates;
- source archive smoke tests;
- Go/C/Lua behavioral benchmark records plus C-native performance and memory
  gates for mature public `liblql` paths.

Fast test command contract:

- `make test` runs the fast C/API test surface and excludes parity-labeled
  transitional Go checks;
- native `lql.unit` coverage must stay effectively immediate and subsecond on
  ordinary developer hardware; if a C unit test needs corpus-scale input,
  sleeps, retries, benchmark timing, release packaging, Go execution, Lua
  execution, or large-fixture generation, it belongs in a narrower explicit
  parity, benchmark, fuzz, package, or release gate instead;
- C-only unit tests may assert streaming and bounded-memory behavior with small
  deterministic fixtures and instrumentation, but must not prove those
  properties by spending noticeable wall-clock time or materializing large
  documents;
- `make parity-test` runs the Go-backed CLI and SDK parity suites explicitly;
- `make test-all` includes both the fast C/API tests and parity tests before
  sanitizer and Lua checks.

Parity benchmark spec:

- `docs/liblql-parity-benchmark-spec.md`

Tests should assert observable behavior, not implementation details.

Parity has two separate meanings in this repository:

- SDK parity: public `liblql` behavior must converge to the Go `pkt.systems/lql`
  library behavior for the LQL language and data transformations. The
  Go-backed parity suite is the oracle for convergence and regression
  detection while the port is incomplete. It must not be treated as C unit
  coverage, and C unit tests must not be generated by mechanically copying Go
  parity rows. C tests own the C-native public contract: handle lifetime,
  ownership, out-parameter state, callback behavior, partial I/O, explicit
  buffered versus streaming APIs, failure diagnostics, cleanup after errors,
  bounded memory, and other behavior that downstream C consumers rely on.
- CLI parity: `clql` must converge to the Go `lql` command's observable CLI
  behavior for supported flags and workflows, excluding prettyx colorized JSON.
  CLI parity tests are command-level oracle checks. They do not prove SDK
  contract coverage unless the same behavior is independently meaningful and
  tested through public liblql APIs.

Both surfaces need executable coverage manifests, but the manifests serve
different purposes. Go-backed manifests classify oracle coverage. C manifests
classify product contract coverage. A claimed public SDK behavior needs
C-native tests for its public C contract; it does not need a C clone of every
Go parity example.

## C-Native Strategy

The port must be a C implementation, not a Go implementation transliterated
into C. Design decisions should favor explicit ownership, streaming data flow,
bounded memory, predictable error surfaces, and use of lonejson's native
streaming/range APIs. Full-document materialization, per-candidate scalar-list
materialization, hidden buffering, and ad hoc JSON handling are implementation
smells unless the public API is explicitly named as buffered and caller-owned.

Development should proceed in larger outcome slices:

- complete a behavior surface, such as selector evaluation, streaming payloads,
  projection, mutation, CLI workflow, Lua facade, or release packaging;
- implement the C-native behavior and its public contract tests together;
- run Go parity as an oracle to catch semantic drift from the reference;
- add or update benchmarks when performance or memory behavior is part of the
  surface;
- commit the completed surface slice.

Do not spend implementation cycles mining Go parity cases solely to duplicate
them in C tests. When Go parity exposes a divergence, fix the C behavior, then
add C tests only for the C API invariant or product behavior that should have
prevented the bug. Exhaustive C coverage means the C product contract is
covered surface by surface; it does not mean every Go oracle row has a
mechanical C twin.

## Performance Strategy

The Go implementation is a semantic reference, not a performance target. A C
port that merely matches Go throughput is suspect unless the benchmark is
dominated by external I/O, process startup, or another documented non-library
cost. `liblql` should normally be substantially faster and more memory-stable
than Go for library-level streaming selector, projection, mutation, and payload
access paths.

Performance gates must therefore validate two things separately:

- behavioral parity: candidates, matches, payload counts, payload bytes, and
  parse/error outcomes agree with the oracle for supported behavior;
- C-native performance: steady-state memory remains independent of total input
  size, candidate size, match count, and result size, and C library paths have
  documented headroom over Go once the relevant feature is mature.

Initial benchmark gates may be behavioral while surfaces are incomplete. Once a
surface is claimed, the benchmark should move to a documented C-native
threshold, baseline, or speedup floor for that specific public path.
CLI-mediated and Lua facade timing may remain report-only where process startup
or facade overhead dominates the measurement, but claimed public liblql and Lua
memory behavior must be gated rather than treated as report-only.

## Packaging Requirements

Packaging follows the pkt.systems lifecycle.

Binary SDK archives must be relocatable and must not contain:

- source checkout paths;
- build paths;
- dependency cache paths;
- `$HOME`;
- absolute local `file://` URLs;
- sanitizer runtime or debug metadata;
- non-relocatable RPATH/RUNPATH or Darwin install names.

`package-verify` must expand checksum-listed artifacts and nested archives
before scanning.

`dist/` is generated output. Checksums/manifests define upload artifacts.

## Iteration Plan

The port should progress in falsifiable slices:

1. Lifecycle foundation
   - CMake/Make/scripts/presets;
   - lonejson dependency acquisition;
   - initial package surfaces.

2. Selector core
   - parse/evaluate scalar selectors;
   - Go parity tests for supported subset.

3. lonejson upgrade integration
   - consume candidate stream with 64-bit ranges;
   - consume path-aware visitor and JSON Pointer helpers;
   - remove liblql-owned path reconstruction and scalar-list materialization
     where possible.

4. Streaming query foundation
   - decision-only candidate stream over `FILE *`;
   - candidate index, offset, and byte size callbacks;
   - no payload capture in decision-only mode.

5. Full selector parity
   - wildcards;
   - temporal selectors;
   - `in`;
   - selector plans.

6. Streaming query parity
   - seekable source range payload handles;
   - non-seekable caller sink/spool payload handles;
   - stop controls.

7. Projection parity
   - field selection;
   - CLI `-f`.

8. Mutation parity
   - parse/plan;
   - multi-path rewrite;
   - file-backed mutation values;
   - inline write behavior.

9. Lua parity
   - facade and tests;
   - parity benchmarks.

10. Packaging completion
   - liblql SDK archives;
   - clql archives;
   - release matrix verification.

Each slice must add or update C-native product tests before claiming support.
Go oracle coverage must also be updated when the slice changes claimed LQL
language, transformation, or CLI compatibility behavior. A slice is not
complete merely because Go parity passes.

## Current Repository Status

Current implementation is an early slice:

- lifecycle scaffold exists;
- lonejson `v0.35.0` binary archive acquisition from GitHub release assets
  exists;
- the public C API exposes an instantiatable receiver shell through `lql_new()`
  and method-pointer dispatch; selector/query/projection/mutation operations
  are implemented by receiver-compatible private functions and are not exported
  as free-function wrappers; every installed receiver data or method field has
  an ownership/error-behavior comment in `include/lql/lql.h`;
- `make test` includes `lql.public-api-style`, which fails if removed
  selector/query/payload/projection/compact/mutation free-operation prototypes
  reappear in the installed header or, when a shared library is built, as
  exported dynamic symbols; for shared builds it also allowlists the complete
  exported `lql_*` symbol set to construction, diagnostics, version, and
  capability helpers only; it also scans project-owned source trees for
  exact-name macro or static wrapper shims that recreate those removed
  operation functions and for static receiver-operation shims that should have
  been folded into the receiver-compatible implementation functions; receiver
  cleanup fields named `*_free`, project-owned cleanup helpers named
  `free_*`/`*_free`, and public allocator wrapper prototypes are also rejected
  so cleanup stays on the `*_destroy` receiver surface and the allocator
  boundary stays internal; the same style gate rejects undocumented public
  receiver fields so the installed SDK surface remains self-describing;
- project-owned allocations have an internal central liblql allocator surface,
  and
  direct C runtime allocation calls are limited to the allocator
  implementation; `make test` enforces this by failing on direct
  `malloc`/`calloc`/`realloc`/`free`/`strdup` calls in project-owned C sources
  outside `src/lql_allocator.c`;
- decision-only candidate streaming over `FILE *` uses lonejson candidate
  streams with `CAPTURE_NONE` and 64-bit candidate ranges;
- seekable `FILE *` range rereads reject offsets that cannot round-trip through
  the platform `off_t`, so large-range failures are explicit instead of
  truncated;
- decision-only `FILE *` query streams expose stop controls for match count,
  candidate count, bytes read, and callback-requested graceful stop;
- decision-only callback-source query streams expose the same decision and
  stop-control behavior over caller-provided read callbacks with no candidate
  payload capture;
- callback-source matched-candidate query streams expose callback-scoped
  `LQL_PAYLOAD_SPOOLED` payload handles and receiver payload writer support for
  non-seekable plus-value access without retaining payloads after the match
  callback returns;
- matched-candidate `FILE *` query streams expose callback-scoped
  `LQL_PAYLOAD_SEEKABLE_RANGE` payload handles, with
  `ctx->payload_write_json()` and `ctx->payload_write_json_sink()` preserving the
  parser source position while rereading the matched candidate range; this path
  still uses candidate `CAPTURE_NONE` and does not retain candidate JSON;
- seekable and spooled payload handles can be written to caller-managed sink
  callbacks through `ctx->payload_write_json_sink()` without requiring a
  `FILE *`;
- selection-mode `clql -M/--matches-only` matches Go CLI behavior by writing
  matched JSON candidates and returning success even when no candidates match;
  mutation-mode `-M` remains the Go-compatible output filter for matched
  candidates only;
- default `clql selector < data.json` output streams non-seekable stdin
  through lonejson candidate parsing with callback-scoped spooled candidate
  payloads, so it no longer reads the complete stdin stream into memory before
  deciding matches;
- `clql -c/--compact selector < data.json` compacts matched non-seekable stdin
  candidates by streaming callback-scoped spooled payloads back through
  lonejson rather than materializing complete candidates in liblql;
- `clql -f/--field selector < data.json` projects matched non-seekable stdin
  candidates by streaming callback-scoped spooled payloads through the public
  projection visitor path, without retaining complete candidates in liblql;
- `clql` accepts an empty selector for match-all file selection, including the
  Go-compatible shorthand where a single existing file path is the input rather
  than selector text;
- `clql` accepts multiple selector arguments, combines them with default AND
  semantics, and honors `--or` / `-O` for OR composition before the final file
  or `-` input argument;
- `clql selector data.json` uses seekable candidate offset/size ranges to
  reread and write matched payloads without full-input materialization;
- `clql -c/--compact selector data.json` compacts matched seekable file ranges
  through lonejson without candidate materialization;
- `clql -f/--field selector data.json` supports root, nested object-field,
  array-index, and escaped JSON Pointer projection on seekable file inputs
  using the public projection API, lonejson path visiting, and writer output;
- the initial C projection API exposes receiver methods
  `ctx->projection_parse()` and `ctx->project_file_range()` for object and
  array-index paths over seekable file ranges, `ctx->project_source()` for
  caller-provided read callbacks, plus `ctx->project_json()` for explicitly
  caller-buffered JSON values;
- the initial C compact API exposes `ctx->compact_file_range()` for streaming
  seekable ranges, `ctx->compact_source()` for caller-provided read callbacks,
  and `ctx->compact_json()` for explicitly buffered JSON values;
- the public C API exposes `lql_version()` and installs generated
  `lql/version.h` version macros, plus `lql_capabilities_get()` for the
  currently implemented public API surfaces, with package and source-archive
  verification proving the generated header and capability query build from
  installed and extracted trees;
- the initial C mutation API exposes receiver methods
  `ctx->mutation_plan_parse()`, `ctx->mutation_plan_parse_with_options()`,
  `ctx->mutation_plan_count()`, and `ctx->mutation_plan_destroy()` for CLI-style
  mutation parse/plan validation;
  file-backed mutation values remain disabled by default and require explicit
  parse options;
- mutation execution APIs expose
  `ctx->mutate_file_range_root_fields()` for bounded source-backed rewrites of
  root object fields and `ctx->mutate_file_range_paths()` for bounded
  source-backed rewrites of concrete object/member paths with optional concrete
  array indexes and existing-position `*` object-child or `[]` array-element
  wildcards over seekable file ranges; existing object-member and array-element
  mutation positions also support `**` one-child and `...` recursive path
  segments; Go-compatible stream mutation treats concrete numeric descendant
  paths under an existing array as object-key creation, so `/items/0=x`
  rewrites `items` as an object member named `"0"` rather than preserving the
  source array, while explicit `[]` wildcard mutation remains array traversal;
  supported set values include `time:` normalization to UTC RFC3339Nano strings
  and `file:/textfile:/base64file:` source-backed file values; public C
  execution is currently available for seekable file ranges,
  seekable candidate streams through `ctx->mutate_file_range_candidates()`,
  projection-before-mutation seekable candidate streams through
  `ctx->mutate_file_range_projected_candidates()`,
  caller-provided read callbacks through `ctx->mutate_source_paths()`,
  callback-source candidate streams through
  `ctx->mutate_source_candidates()`, projection-before-mutation callback-source
  candidate streams through `ctx->mutate_source_projected_candidates()`, and
  explicitly caller-buffered JSON values through `ctx->mutate_json()`;
- `clql -m/--mutate` emits all seekable file candidates in mutation mode,
  applies supported concrete-path, existing-position wildcard, and
  existing-position recursive mutations to matched candidates;
- public SDK seekable and callback-source candidate-stream mutation exposes
  the selector-plus-plan path used by `clql`: top-level arrays are expanded as
  candidate streams, matched candidates are mutated, unmatched candidates are
  preserved unless `matches_only` is set, and result counters report candidates
  and matches;
- brace shorthand mutation execution is covered by C SDK contract tests and
  Go-backed CLI parity tests for nested set, increment, delete, and path
  expansion behavior;
- escaped JSON Pointer mutation paths are covered by C SDK contract tests and
  Go-backed CLI parity tests for set, delete, and nested path expansion
  behavior;
- quoted mutation values are covered by C SDK contract tests and Go-backed CLI
  parity tests; JSON-looking quoted values follow Go typing behavior, so quoted
  numeric, boolean, and null literals are emitted as JSON values rather than
  strings while ordinary escaped text remains a JSON string;
- mutation parser and execution coverage includes Go-compatible decrement and
  signed-delta increment spellings plus `rm:`, `remove:`, `delete:`, and `del:`
  delete aliases;
- `clql -m/--mutate selector < data.json` applies the same supported streaming
  mutation subset to matched non-seekable stdin candidates through
  callback-scoped spooled payloads, preserves unmatched candidates by default,
  and honors `-M/--matches-only` without materializing the full input stream;
- `clql -m/--mutate -f/--field` follows Go CLI order for seekable file input
  and non-seekable stdin: project each output candidate first, then mutate the
  projected value only for matched candidates; this path uses callback-scoped
  temp-file spill for the projected candidate and does not retain projected
  JSON in memory;
- `clql -m/--mutate` accepts a single file argument with no selector as
  match-all mutation input and rejects multiple file inputs explicitly;
- match-all `clql -m/--mutate` over mixed candidate streams preserves
  non-object candidates unchanged while applying field mutations to object
  candidates;
- `clql -F/--enable-file-mutations` opts into parsing file-backed mutation
  values and supports `file:`, `textfile:`, and `base64file:` streaming
  execution over seekable file input and supported spooled stdin mutation,
  including `~/` home-directory expansion for file-backed value paths;
- `clql -m -M/--matches-only` emits only matched seekable file candidates after
  applying supported concrete-path mutations;
- `clql -m -i/--inline` and `clql -m -w/--write` rewrite a single seekable
  input file through a sibling temp file and rename only after successful
  mutation; stdin and non-mutation inline use are rejected;
- current selector subset evaluation uses lonejson path-aware visitor callbacks
  and marks selector term hits as values stream through, rather than building a
  per-candidate scalar document list;
- first C selector parse/evaluate subset exists, including
  `contains.any`, `icontains.any`, `in.any`, wildcard selector paths, and
  bracket-sugar array paths, string-term `ignoreCase`/`ic` flags, and
  multi-bound numeric ranges, with omitted string-term values treated as path
  assertions that include JSON `null`, while explicit `exists`, `eq`, `in`, and
  valued string terms do not treat JSON `null` as an empty string or non-null
  value; temporal `date` terms, datetime `range` bounds, and relative
  `date.since` macros are implemented; nested indexed `and.N` / `or.N`
  logical wrapper groups, including `and.or.N` and `or.and.N` wrapper chains,
  are parsed and evaluated recursively; shorthand selectors tolerate whitespace
  around comparison operators, and brace selector assignments accept comma,
  newline, or whitespace-separated `key=value` clauses;
- selector parse-error parity tests cover supported-term key validation,
  duplicate-key validation, strict `in.any` value whitespace validation, and
  invalid selector invariants;
- `clql` exists as a selector/projection/mutation compatibility CLI with
  manifest-checked Go-backed parity coverage for selector composition,
  streaming selection, projection, mutation, inline/write modes, option
  parsing, malformed JSON diagnostics, and file-backed mutation workflows;
- fast CTest now includes `lql.cli-smoke`, which asserts the stable
  `clql --version` format and the documented `clql --help` option surface;
- C SDK contract tests cover the currently implemented public liblql selector,
  streaming, projection, compacting, mutation, version, and capability
  surfaces, including mutation plan parse success, expansion counts, default
  and explicit file-backed parse options, file-backed mutation value execution
  over buffered and seekable file-range APIs, source-backed projection,
  compacting, and mutation over fragmented caller reads, source callback read
  failures with empty failed-output state, mutation parse-error invariants,
  parser failure output-handle clearing, projection invalid-argument
  `out_found` state, and query callback failure status propagation. Query and
  candidate-stream mutation
  invalid-argument failures now also assert a safe zeroed `lql_query_result`
  output state. Decision and match callback failures now assert actionable
  diagnostics and partial result-counter propagation for seekable and
  callback-source query paths; callback-source decision and
  spooled match queries preserve partial result counters when a reader fails
  after an emitted candidate, matching the callback-source candidate mutation
  contract. Handle-producing selector, projection, and mutation APIs also have
  C-only ownership contract tests for optional diagnostics, output-handle
  clearing on parse failure, empty-selector ownership, and `NULL` cleanup/count
  behavior. An SDK coverage manifest ties claimed C contract surfaces to C
  unit functions. Further C test work should be driven by newly claimed API
  contracts or defects rather than by mechanically copying Go parity rows;
- Go-backed parity tests exist for the current CLI surface and remain a
  transitional oracle for semantic convergence; they run under the explicit
  `make parity-test` target and broader gates, not the fast `make test`
  target. They must not be counted as C SDK unit tests and must not drive
  mechanical duplication of Go rows into C tests;
- CLI malformed JSON execution is covered by Go-backed parity tests over stdin
  and seekable file inputs for selection, matches-only selection, compact
  output, projection, and mutation, with exit-code and diagnostic assertions;
- CLI pflag-compatible long boolean value forms are covered for existing
  boolean flags that affect observable behavior, including
  `--compact=true`, `--matches-only=true|false`, `--or=true|false`, and
  `--enable-file-mutations=true|false`;
- CLI pflag-compatible short option clusters are covered for supported
  shorthand flags, including boolean clusters, short boolean `=true|false`
  values, and clustered `-f`, `-m`, and `-t` value forms;
- CLI pflag-compatible `--` end-of-options handling is covered so
  dash-prefixed positional input paths can be selected after the terminator;
- CLI non-inline mutation supports multiple seekable input files and mixed
  file/stdin inputs in argument order, matching the Go CLI's
  `splitMutationArgs` behavior while preserving inline mode's single-file
  restriction;
- CLI inline mutation input rejection matches the Go CLI distinction between
  missing file path and invalid single-file input forms such as stdin or
  multiple files;
- Go-backed SDK parity tests now exist for public `liblql` selector
  parse/evaluate behavior, projection parser failures, buffered and seekable
  file-range JSON projection including malformed JSON execution errors,
  mutation plan parsing and parse failures, buffered and seekable file-range
  JSON mutation including file-backed mutation values and malformed JSON
  execution errors, compact serialization, compact error behavior, and current
  streaming query behavior through the receiver C API, comparing
  `ctx->selector_parse()`, `ctx->selector_parse_or()`,
  `ctx->matches_json()`, `ctx->project_json()`, `ctx->project_source()`,
  `ctx->project_file_range()`, `ctx->mutation_plan_parse()`,
  `ctx->mutation_plan_parse_with_options()`, `ctx->mutation_plan_count()`,
  `ctx->mutate_json()`, `ctx->mutate_file_range_root_fields()`,
  `ctx->mutate_file_range_paths()`,
  `ctx->mutate_file_range_candidates()`, `ctx->mutate_source_paths()`,
  `ctx->mutate_source_candidates()`, `ctx->compact_json()`,
  `ctx->compact_source()`,
  `ctx->compact_file_range()`, `ctx->query_file_decisions()`,
  `ctx->query_source_decisions()`, `ctx->query_file_matches()`, and
  `ctx->query_source_spooled_matches()` against the pinned Go library or
  standard compact JSON behavior over the current selector, projection
  success/error, mutation success/error, compact success/error, and stream
  corpora; this is behavioral oracle coverage, not C SDK unit coverage and not
  a requirement that the C API mirror Go API shape;
- C SDK projection tests assert duplicate projection paths are
  idempotent and parent/child projection path conflicts are rejected through
  the public projection API;
- C SDK projection tests assert the current projection parser failure
  corpus, including empty, blank, root, missing-leading-slash, leading-index,
  and oversized-index field sets;
- C SDK compact tests assert the current malformed JSON corpus through
  buffered, source-backed, and seekable file-range public compaction APIs;
- C SDK streaming tests assert the current malformed JSON corpus across
  seekable file decision streams, seekable file payload streams,
  callback-source decision streams, and callback-source spooled payload streams;
- C SDK payload tests assert callback-scoped seekable and callback-source
  spooled payloads can be written through caller-managed sink callbacks,
  including sink failure propagation, source position restoration after
  seekable payload writes, unrepresentable seekable payload offset failure, and
  partial result accounting;
- SDK streaming parity currently asserts candidate counts, match counts,
  consumed byte counts, stop state/reason, callback counts, seekable/spooled
  payload kinds, decoded matched payload JSON collected through the public
  payload sink API, and malformed JSON stream errors. Full non-stopped streams
  report consumed input bytes, including
  trailing delimiters, while early-stop streams retain candidate-end accounting
  for stop decisions;
- C SDK unit coverage is manifest-checked: every `expect_* (void)` SDK
  unit group in `tests/test_lql.c` must have exactly one manifest entry and
  exactly one `main()` call, so C-only regressions cannot be added without
  executable coverage accounting;
- the Go-backed SDK parity suite is intentionally excluded from sanitizer CTest
  presets because the cgo test process cannot reliably load an
  ASan-instrumented shared liblql with the ASan runtime first; project-owned C
  unit tests remain the sanitizer authority for SDK behavior;
- the Lua host smoke test is also excluded from sanitizer CTest presets because
  the system Lua executable loads before the ASan runtime; non-sanitizer
  `make test`, `make lua-test`, package verification, and release Lua artifact
  checks remain the Lua facade authorities;
- the Lua tree includes a Lua 5.5 facade over a direct `lql.core` C module
  linked against shared liblql and implemented through public liblql headers;
  `lql.new()` returns a C-owned client userdata backed by a public `lql *`
  receiver, with deterministic smoke tests for selector decisions, selection
  output, callback decision streams, callback-scoped seekable payload handles,
  callback-scoped payload streaming through `match.write_json(callback)`, file
  and buffered-JSON projection, file and buffered-JSON mutation, query stop
  reasons, candidate/match limit options, expired payload handles, callback
  error propagation, and structured errors; the public API style gate rejects
  Lua facade use of private liblql headers, `LQL_INTERNAL_SYMBOL`, or private
  `_impl` receiver implementation functions so Lua remains a public-header
  binding rather than a private in-process shortcut;
- the parity benchmark surface now has Go, C, and Lua runners over the shared
  generated fixture matrix; the Lua runner loads `lua/lql.lua` and uses the
  direct `lql.core` module rather than shelling out to `clql`; Go helper
  records, C native helper records, and Lua facade runner records now report
  `ns_per_op`, Lua plus-value benchmark modes count bytes through
  `match.write_json(callback)` instead of `match.json()` materialization, while
  Go and C helper records also report OS `getrusage` peak RSS as
  `peak_rss_bytes` for the benchmark schema and Lua records report process peak
  RSS when host `time` support is available; `make bench-check` enforces a
  supported-C smoke RSS ceiling through `LQL_BENCH_MAX_C_PEAK_RSS_BYTES`
  defaulting to 128 MiB; `make bench-memory-check` adds a separate scalable
  Go/C/Lua streaming profile that generates at least 16 MiB of NDJSON by
  default, compares C and Lua counters against Go, requires Lua peak RSS, and
  can be scaled with `LQL_BENCH_MEMORY_COUNT` and
  `LQL_BENCH_MEMORY_BLOB_BYTES`, moving the benchmark surface toward the final
  1 GiB/128 MiB proof without slowing the normal smoke gate;
- current local lifecycle confidence has passed `make test-all`,
  `make bench-check`, `make bench-memory-check`, `make package-verify`, and
  `make release-matrix` on the available host/toolchain set. The release matrix
  builds and verifies all Linux GNU/musl targets in the configured matrix and
  reports Darwin as skipped when the local Darwin target compiler cannot link.
  These gates are strong evidence for the current implementation state, but
  they are not a substitute for a requirement-by-requirement completion audit
  before claiming full LQL parity or final release readiness;
- `make release` is the final local release gate. It cleans generated state,
  runs the full local test gate, benchmark smoke gate, scalable memory
  benchmark gate, release target matrix, package verification, and checksum
  manifest verification. The `lql.release-surface` CTest check rejects a
  placeholder release target and is included in source-archive verification so
  extracted release sources preserve the same lifecycle surface;
- host `liblql` and `clql` package archive production exists through
  `scripts/package.sh`, with checksum, layout, privacy, and ELF runtime-path
  verification plus extracted host direct, CMake `find_package`, and
  pkg-config consumer smokes;
- standalone Lua source package production now writes
  `dist/liblql-lua-<version>.tar.gz` with `VERSION`, exact
  `RELEASE_MANIFEST`, Lua sources, tests, benchmark runner, rockspec template,
  and Lua release scripts; `make release-lua-artifacts` also renders
  `dist/liblql-<version>-1.rockspec` with a public release URL and builds
  `dist/liblql-<version>-1.src.rock` through LuaRocks from the staged source
  package. Package verification checks checksum coverage, layout, manifest
  exactness, absence of C SDK payloads, local path privacy, release-safe
  rockspec URLs, and the nested Lua source package inside the source rock;
- source archive production exists with injected `VERSION`, `RELEASE_MANIFEST`,
  exact manifest verification, and extracted-tree configure/build/test smoke;
- host `clql` archive production carries required lonejson runtime libraries
  and verifies extracted `clql --version` through a relocatable runpath;
- `make release-matrix` selects target-correct Linux compilers, acquires the
  matching lonejson SDK archive for each target, builds and verifies
  `liblql` and `clql` artifacts for `x86_64`, `aarch64`, and `armhf`
  GNU/musl targets, and fails package verification if packaged shared
  libraries or `clql` binaries do not match their target architecture;
  Darwin packaging remains conditional on a working target compiler/linker.

The repository must not claim full LQL parity until the verification gates prove
it.

## Non-goals

- Do not reimplement `pkt.systems/prettyx` colorized JSON.
- Do not vendor the adjacent Go source checkout.
- Do not add hidden full-message buffering behind streaming-looking APIs.
- Do not implement bespoke JSON parser/tokenizer/serializer logic in liblql.
- Do not hide unsupported behavior by omitting tests or benchmark cases.
