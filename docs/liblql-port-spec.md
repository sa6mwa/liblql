# liblql Port Specification

## Goal

Port `pkt.systems/lql v0.17.1` from Go to C89 in this repository as
`liblql`, with a companion CLI named `clql`.

The C port must be suitable for lockd-style local JSON search and mutation
workloads. It must preserve observable LQL behavior from the Go implementation
where claimed, prove parity through executable tests and benchmarks, and use
`lonejson` for JSON parsing, serialization, validation, stream framing,
escaping, payload capture, and rewrite support.

The implementation must not reference the adjacent Go source checkout from
repository files. Go parity must go through the `parity/` Go module, which pins
`pkt.systems/lql v0.17.1`.

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

The API should be handle-oriented and explicit about ownership:

- parser/compiled selector handles are owned by the caller and freed with
  liblql cleanup functions;
- result payload handles are valid only for documented callback lifetimes and
  must not imply retained candidate copies;
- all project-allocated strings or buffers are released through liblql cleanup
  functions;
- error messages are actionable and available through explicit error objects.

The API should eventually expose these surfaces:

- selector parse and free;
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

## Verification Requirements

Verification is the primary quality gate.

Required gates:

- C unit tests for every public behavior;
- public header standalone compile tests;
- C89 consumer tests;
- CMake install-tree consumer tests;
- pkg-config consumer tests;
- `clql` smoke tests;
- Go parity tests through `parity/`;
- Lua parity tests when Lua facade exists;
- sanitizer tests;
- package verification and privacy/relocatability gates;
- source archive smoke tests;
- Go/C/Lua parity benchmarks.

Parity benchmark spec:

- `docs/liblql-parity-benchmark-spec.md`

Tests should assert observable behavior, not implementation details.

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

Each slice must add or update parity tests before claiming support.

## Current Repository Status

Current implementation is an early slice:

- lifecycle scaffold exists;
- lonejson `v0.35.0` binary archive acquisition from GitHub release assets
  exists;
- decision-only candidate streaming over `FILE *` uses lonejson candidate
  streams with `CAPTURE_NONE` and 64-bit candidate ranges;
- seekable `FILE *` range rereads reject offsets that cannot round-trip through
  the platform `off_t`, so large-range failures are explicit instead of
  truncated;
- decision-only `FILE *` query streams expose stop controls for match count,
  candidate count, bytes read, and callback-requested graceful stop;
- `clql -M/--matches-only` uses the decision-only streaming path over stdin
  and does not materialize candidate payloads or write matched JSON;
- `clql` accepts an empty selector for match-all file selection, including the
  Go-compatible shorthand where a single existing file path is the input rather
  than selector text;
- `clql selector data.json` uses seekable candidate offset/size ranges to
  reread and write matched payloads without full-input materialization;
- `clql -c/--compact selector data.json` compacts matched seekable file ranges
  through lonejson without candidate materialization;
- `clql -f/--field selector data.json` supports root, nested object-field,
  and array-index projection on seekable file inputs using the public
  projection API, lonejson path visiting, and writer output;
- the initial C projection API exposes `lql_projection_parse()` and
  `lql_project_file_range()` for object and array-index paths over seekable
  file ranges;
- the initial C compact API exposes `lql_compact_file_range()` for streaming
  seekable ranges and `lql_compact_json()` for explicitly buffered JSON values;
- the initial C mutation API exposes `lql_mutation_plan_parse()`,
  `lql_mutation_plan_parse_with_options()`, `lql_mutation_plan_count()`, and
  `lql_mutation_plan_free()` for CLI-style mutation parse/plan validation;
  file-backed mutation values remain disabled by default and require explicit
  parse options;
- mutation execution APIs expose
  `lql_mutate_file_range_root_fields()` for bounded source-backed rewrites of
  root object fields and `lql_mutate_file_range_paths()` for bounded
  source-backed rewrites of concrete object/member paths with optional concrete
  array indexes and existing-position `*` object-child or `[]` array-element
  wildcards over seekable file ranges; existing object-member and array-element
  mutation positions also support `**` one-child and `...` recursive path
  segments;
  immediate concrete child mutations under arrays follow Go stream behavior by
  replacing the array value with an object keyed by the requested numeric
  segments; supported set values include `time:` normalization to UTC
  RFC3339Nano strings and `file:/textfile:/base64file:` source-backed file
  values; non-seekable execution still returns unsupported;
- `clql -m/--mutate` emits all seekable file candidates in mutation mode,
  applies supported concrete-path, existing-position wildcard, and
  existing-position recursive mutations to matched candidates, and leaves
  non-seekable mutation behavior unsupported;
- `clql -m/--mutate` accepts a single file argument with no selector as
  match-all mutation input and rejects multiple file inputs explicitly;
- `clql -F/--enable-file-mutations` opts into parsing file-backed mutation
  values and supports `file:`, `textfile:`, and `base64file:` streaming
  execution;
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
  assertions; temporal `date` terms, datetime `range` bounds, and relative
  `date.since` macros are implemented;
- selector parse-error parity tests cover supported-term key validation,
  duplicate-key validation, and invalid selector invariants;
- `clql` exists as a minimal selector smoke CLI; default matched-JSON output
  from stdin still uses buffered input until non-seekable plus-value payload
  handles are implemented, and projection path behavior is still being expanded
  toward full parity;
- Go parity tests exist for the initial selector subset;
- package archive production is scaffolded, not complete.

The repository must not claim full LQL parity until the verification gates prove
it.

## Non-goals

- Do not reimplement `pkt.systems/prettyx` colorized JSON.
- Do not vendor the adjacent Go source checkout.
- Do not add hidden full-message buffering behind streaming-looking APIs.
- Do not implement bespoke JSON parser/tokenizer/serializer logic in liblql.
- Do not hide unsupported behavior by omitting tests or benchmark cases.
