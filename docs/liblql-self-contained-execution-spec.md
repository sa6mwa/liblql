# liblql Self-Contained Direct Execution Specification

## Authority

This is the governing architecture for the v0 rewrite. It supersedes the
LoneJSON-backed application architecture in
`docs/liblql-direct-execution-spec.md`. The selector AST grammar and Go
v0.17.1 observable behavior remain authoritative.

liblql is self-contained for direct application. It does not include
`lonejson.h`, link `liblonejson`, or use LoneJSON at runtime. LoneJSON may be
kept in the repository temporarily as an implementation reference and remains
an independent upstream component for other consumers; it is not a liblql
runtime dependency or part of liblql's shipped ABI.

The motivation is architectural, not a language claim. The Go reference owns
one scanner state that performs strict framing, structural validation, selector
observation, capture pruning, and compact emission. A separately compiled
event parser and writer introduce a boundary in precisely that hot path. The C
implementation must instead own one equivalent state machine and prove every
performance claim with profiling and paired Go/C benchmarks.

## Non-Negotiable Contract

- ANSI C89, with GCC as the authoritative performance compiler.
- GCC-only performance evidence. Do not run or maintain Clang comparison rows
  for this rewrite; upstream release toolchains are Bootlin GCC.
- `lql_stream_apply` is the sole true-streaming apply entry point.
  `lql_stream_apply_spooled` is an explicitly named compatibility API for
  operations that still require a current-record materialization.
- Strict NDJSON only. Root arrays are hard errors and are never flattened. This
  intentionally diverges from Go lql's array-document stream behavior to
  preserve the NDJSON contract. Scalar roots are validated records and do not
  match a non-empty selector.
- Input records may use any JSON-permitted whitespace around structural tokens;
  compact JSON is an output normalization, never an input precondition.
- Every emitted record is compact JSON plus one newline.
- Earlier completed records remain observable if a later record is malformed.
- Duplicate object keys are observed in source order.
- The existing receiver API, selector AST, temporal behavior, output modes,
  limits, callback semantics, and accepted Go v0.17.1 parity requirements remain
  in force unless this document names a replacement. JSON scalar equality is the
  documented liblql divergence: typed numbers, booleans, and null are verified
  by C behavior tests rather than by pinned-Go parity rows. Projection and
  mutation remain unavailable from the true-streaming API until they have
  incremental emitters; callers needing their existing semantics must opt into
  the named spooled API.
- Live heap for `lql_stream_apply` remains at or below 256 KiB, independent
  of total input, records, matches, and repeated executions. The true-streaming
  API never materializes a record or writes input to disk. It rejects plans
  wider than its bounded one-pass scanner before consuming input. The explicitly
  named spooled compatibility API may retain one current record and spill it to
  a temporary file. Every `lql_new` receiver has one independent 8 MiB budget
  across parsed handles, temporary apply plans, and compatibility spools.
- GCC Go/C speedup is at least 1.0x on the hard performance gate's
  representative accepted rows. Profile before optimizing; do not add caches or
  special cases based only on benchmark deltas.
- `make sdk-parity-gate` is the fast GCC-only liblql SDK parity gate. It uses
  the public C receiver API through the `lql_direct_bench` harness, includes
  warmup and steady-state Go/C records, forbids unsupported rows, validates
  Go/C counters for every emitted row, covers selector text and selector AST
  JSON through public SDK parsers, and enforces C RSS below Go. Hard timing is
  owned by `make direct-perf-gate`.
- `make direct-parity-matrix` is the broader GCC-only SDK accepted-row gate. It
  extends coverage across source/file callbacks, projection, mutation families,
  temporal, array, range, indexed, selector AST JSON, and large-record cases.
  It is a semantic parity gate: unsupported rows and counter/output mismatches
  are failures. Release rehearsal runs it after `make test-all` so the release
  proof graph cannot pass on the fast SDK gate alone. `make perf-gate` owns hard
  C-vs-Go speed/RSS enforcement.
- `make direct-profile-hotspots` is the bounded C profiling gate before
  performance-directed scanner/emitter changes. It profiles the currently
  tight GCC rows and writes perf reports under `build/direct-profiles/`.

## Execution State Machine

`lql_stream_apply` owns a single `lql_json_scan` state for the whole input
and one resettable `lql_json_candidate` state for the current record.

```text
reader → bounded byte buffer → strict JSON scanner
                                ├─ selector observer
                                ├─ caller-owned compact source-range replay
                                ├─ record framing, limits, diagnostics
                                └─ explicit spooled compatibility adapter
```

The scanner is not a general DOM parser. It is a pull scanner with bounded
input and token scratch storage. It consumes JSON-permitted whitespace at every
grammar boundary, then recognizes and validates JSON structure, strings,
escapes, Unicode surrogate pairs, raw UTF-8, literals, and numbers while the
candidate state owns LQL decisions. Raw invalid UTF-8 in a JSON string is a
JSON error, even though some Go decoder paths replacement-map it.

For every token, the direct hot path performs only the work required by the
compiled program:

1. Consume and validate the token.
2. Update path/container and selector state when the token can affect a
   predicate.
3. At the completed root, finalize selection and invoke decisions. Selected
   output and value callbacks replay a validated compact source range supplied
   by the caller; no record bytes are retained by `lql_stream_apply`.
4. Once a monotonic selector state proves a matched-only record cannot match,
   use structural skip routines for irrelevant descendants.
5. Reset candidate state and continue. Projection, mutation, and compacting
   non-compact selected records require either a future incremental emitter or
   the explicitly requested spooled compatibility API.

This mirrors the Go reference's fast object path. It is not a callback adapter
around a generic JSON transform engine and it must not reintroduce Candidate
Run, result caches, or a whole-input buffer.

## Scanner Components

The implementation is split by stable responsibility:

- `lql_json_reader`: bounded reader callback adapter, byte offsets, lookahead,
  strict NDJSON separators, and byte-limit precedence.
- `lql_json_scan`: JSON grammar, fast unescaped-string spans, decoded-string
  fallback, number validation, structural stack, and skip routines.
- `lql_json_emit`: compact JSON escaping and punctuation emission to an
  internal sink. It is used directly by the scanner; there is no event visitor
  boundary in the hot path.
- `lql_json_spool`: private implementation of the explicitly named
  `lql_stream_apply_spooled` compatibility API. It is never entered from
  `lql_stream_apply`.
- `lql_stream`: selector compilation, candidate decisions, callbacks, limits,
  projection/mutation composition, and error translation.

All components are private. The installed public header is the API authority;
it documents the separate true-streaming and explicitly spooled application
contracts.

## Capture Policy

`lql_stream_apply` has no payload capture policy. Decision-only application
reads and validates through bounded scanner state. Selected output and value
callbacks require caller-declared compact input and a source-range writer; the
scanner validates the record first, then replays that caller-owned range.

`lql_stream_apply_spooled` has explicit payload capture for selected output,
value callbacks, or semantic delayed-output operations. It may spill records,
so its API name and documentation must remain precise.

The first proof path is one direct top-level string equality selector with a
value callback and caller-owned compact range. It must cover dense and sparse
records, fragmented reads, escaped strings, a later malformed record, and
callback stop/error. The spooled compatibility proof separately covers its
100 MiB spill behavior.

## Cutover Sequence

1. Add scanner unit tests that establish strict JSON acceptance and compact
   emission independently of selectors. Include whitespace-heavy and compact
   input forms, strings/escapes, numbers, nesting, duplicate keys, fragmented
   input, scalar roots, root-array errors, and malformed-input offsets.
2. Implement the Go-inspired fast top-level object equality scanner with
   matched-only callback capture. Run focused GCC/Go benchmarks and a
   C leaf-function profile before extending it.
3. Move decision-only direct selectors onto the scanner, then add path-aware
   selector observation for nested, wildcard, recursive, numeric, and temporal
   terms.
4. Move selected output and callbacks onto validated caller-owned compact
   source ranges; delete the LoneJSON rewriter, event tape, and candidate
   stream paths.
5. Port projection and mutation onto scanner-owned incremental emitters while
   preserving original-record selection and projection-before-mutation order.
   Until then, retain them only through the explicitly named spooled API.
6. Remove LoneJSON includes, CMake targets/linkage, tests, vendored delivery
   assumptions, and all `lql_lonejson_*` helpers. Verify the installed static
   and shared liblql link no LoneJSON SONAME or archive.

Each completed slab is committed only after observable tests and its targeted
benchmark proof pass. A slower proof is reverted; it is not carried as a
fallback path.

Rejected proof paths are part of the performance record. In particular,
direct numeric-lexeme equality collection and raw bool/null literal copying
looked plausible from focused scalar profiles, but both pushed another
accepted matrix row below 1.0x under GCC. Do not reintroduce those
micro-optimizations unless the design changes enough to preserve the full
accepted-row matrix, not just the focused scalar row.

Likewise, generalized first-byte rejection before `memcmp` in streaming string
span matching and nested key matching reduced the visible `memcmp` sample share
on the realworld multi-clause profile, but A/B focused GCC runs made the
current worst C row slower. Keep the committed top-level plain-key first-byte
reject, but do not broaden that pattern back into value spans or nested key
segments without a new profile and full accepted-row matrix proof.

## Verification

The required proof is cumulative:

- direct reset gate and strict C89 warning-clean GCC builds;
- scanner behavior tests and public receiver tests;
- Go/C output parity and paired counters for accepted parity rows; JSON scalar
  equality rows that exercise liblql's typed numeric/boolean/null semantics are
  covered by liblql behavior tests instead of Go parity because they
  intentionally diverge from the pinned Go selector-string coercion behavior;
- GCC focused benchmark gate for representative accepted rows, then the
  release-gated accepted-row matrix;
- Callgrind or equivalent leaf attribution for any hot-path optimization;
- Massif live-heap gate for decision, source-range callbacks, sparse matches,
  100 MiB records, and repeated execution. Spooling is verified separately as
  an explicitly materialized compatibility behavior;
- `readelf` or platform equivalent proving liblql has no LoneJSON dependency.

Selected-record writer output is not currently a paired Go/C benchmark row.
Go v0.17.1 `QueryStreamRequest` exposes decision callbacks and plus-value
callbacks/payload sinks, but it does not expose a query writer mode equivalent
to liblql `LQL_STREAM_OUTPUT_SELECTED_RECORD`. Cover selected-record output
with direct receiver behavior tests, live-heap gates, and C profiling when it
is hot. Do not model it as a Go plus-value callback benchmark row; that measures
callback payload delivery, not selected-output writer emission.

No performance result is accepted solely because C is faster on one host or
because an aggregate median improves.
