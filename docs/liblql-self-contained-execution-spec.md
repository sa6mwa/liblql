# liblql Self-Contained Direct Execution Specification

## Authority

This is the governing architecture for the v0 rewrite. It supersedes the
LoneJSON-backed execution architecture in
`docs/liblql-direct-execution-spec.md`. The selector AST grammar and Go
v0.17.1 observable behavior remain authoritative.

liblql is self-contained for direct execution. It does not include
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
- One public execution entry point: `lql_stream_execute`.
- Strict NDJSON only. Root arrays are hard errors. Scalar roots are validated
  records and do not match a non-empty selector.
- Input records may use any JSON-permitted whitespace around structural tokens;
  compact JSON is an output normalization, never an input precondition.
- Every emitted record is compact JSON plus one newline.
- Earlier completed records remain observable if a later record is malformed.
- Duplicate object keys are observed in source order.
- The existing receiver API, selector AST, temporal behavior, output modes,
  limits, callback semantics, and Go v0.17.1 parity requirements remain in
  force unless this document names a replacement.
- Live heap remains at or below 256 KiB, independent of total input, records,
  matches, and repeated executions. A callback or semantic delayed-output path
  may use one current-record spool with a bounded in-memory prefix and
  file-backed spill.
- GCC Go/C speedup is at least 1.0x on every accepted row. Profile before
  optimizing; do not add caches or special cases based only on benchmark deltas.

## Execution State Machine

`lql_stream_execute` owns a single `lql_json_scan` state for the whole input
and one resettable `lql_json_candidate` state for the current record.

```text
reader → bounded byte buffer → strict JSON scanner
                                ├─ selector observer
                                ├─ compact payload emitter / spool
                                ├─ projection or mutation state
                                └─ record framing, limits, diagnostics
```

The scanner is not a general DOM parser. It is a pull scanner with bounded
input and token scratch storage. It consumes JSON-permitted whitespace at every
grammar boundary, then recognizes and validates JSON structure, strings,
escapes, Unicode surrogate pairs, literals, and numbers while the candidate
state owns LQL decisions.

For every token, the direct hot path performs only the work required by the
compiled program:

1. Consume and validate the token.
2. Update path/container and selector state when the token can affect a
   predicate.
3. If payload capture remains enabled, emit the token's compact form to the
   current spool or output sink.
4. Once a monotonic selector state proves a matched-only record cannot match,
   disable capture and use structural skip routines for irrelevant descendants.
5. At the completed root, finalize selection, invoke decision/value callbacks,
   emit selected output when requested, reset all candidate state, and continue.

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
- `lql_json_spool`: one resettable callback/delayed-output payload container.
  It retains only a bounded memory prefix and spills larger current records.
- `lql_stream`: selector compilation, candidate decisions, callbacks, limits,
  projection/mutation composition, and error translation.

All components are private. The public receiver API is unchanged during the
cutover.

## Capture Policy

Payload capture is explicit and only enabled for selected output, a value
callback, or a semantic delayed-output operation.

For matched-only object selection, capture begins conservatively. The scanner
disables it as soon as the compiled selector can no longer match the current
object root. It continues strict structural validation without emitting or
retaining irrelevant bytes. A matched record retains a compact current-record
payload until its callback returns; an unmatched record is released before the
next record.

The first proof path is one direct top-level string equality selector with a
value callback. It must cover dense and sparse records, fragmented reads,
escaped strings, a later malformed record, callback stop/error, and the 100
MiB spill case.

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
4. Move selected output and callbacks fully onto scanner-owned compact capture;
   delete the LoneJSON rewriter, event tape, candidate stream, and spool paths.
5. Port projection and mutation onto scanner-owned emit/spool primitives while
   preserving original-record selection and projection-before-mutation order.
6. Remove LoneJSON includes, CMake targets/linkage, tests, vendored delivery
   assumptions, and all `lql_lonejson_*` helpers. Verify the installed static
   and shared liblql link no LoneJSON SONAME or archive.

Each completed slab is committed only after observable tests and its targeted
benchmark proof pass. A slower proof is reverted; it is not carried as a
fallback path.

## Verification

The required proof is cumulative:

- direct reset gate and strict C89 warning-clean GCC builds;
- scanner behavior tests and public receiver tests;
- Go/C output parity and paired counters;
- GCC focused benchmark gate for every accepted row, then the full matrix;
- Callgrind or equivalent leaf attribution for any hot-path optimization;
- Massif live-heap gate for decision, callback capture, projection, mutation,
  sparse matches, 100 MiB records, and repeated execution;
- `readelf` or platform equivalent proving liblql has no LoneJSON dependency.

No performance result is accepted solely because C is faster on one host or
because an aggregate median improves.
