# LoneJSON CR: Single-Pass Candidate Transform Visitors

## Intent

liblql needs a lonejson-owned candidate transform surface that validates and
frames each candidate once, lets liblql observe selector state during that same
parse, and writes projected or mutated output from the same validated token
stream. The intent is to remove the current non-seekable dense-transform
spool/replay pass without adding caches, hidden buffering, a second JSON parser,
or liblql-specific behavior to lonejson.

This is a performance and streaming correctness feature. It is not a request
for byte-identical Go JSON output and not a request for lonejson to understand
LQL selectors, projections, or mutations.

## Current Problem

For callback-source candidate streams, liblql cannot seek back to a candidate.
Current projection and candidate mutation therefore use lonejson spooled
candidate capture:

1. lonejson parses the source candidate stream and emits path-aware callbacks.
2. liblql evaluates the selector from those callbacks.
3. lonejson spools candidate bytes so a matched candidate can be replayed.
4. liblql reparses the spooled candidate through projection or mutation writer
   paths.

That shape is bounded and correct, but dense or all-match transforms pay for
spooling, allocation growth, replay, and a second parse for every candidate.
Local release profiles on liblql with lonejson `v0.35.2` show dense mutation
and projection dominated by `lonejson_spooled_append`, `realloc`, memory copy,
and replay/write work rather than liblql selector or mutation dispatch.

Seekable input does not need this feature for payload reread: liblql can use
the 64-bit candidate offset and byte size reported by lonejson. This CR is for
non-seekable callback sources and other source types where the caller cannot
rewind.

## Required Behavior

Add a public candidate-stream transform API, or an equivalent public visitor
composition API, with these semantics:

- The parser validates and frames each candidate exactly once.
- The same candidate parse can drive at least two consumers:
  - a path-aware observer used by liblql to evaluate selector state;
  - a transform writer used by liblql to pass through, suppress, or replace
    values while producing valid JSON output.
- The transform writer preserves lonejson ownership of JSON serialization:
  object/array structure, separators, string escaping, number/token validation,
  and error reporting remain lonejson responsibilities.
- The transform side can pass through the current value, suppress the current
  value, or replace the current value with caller-provided JSON/string/number/
  boolean/null output according to callback decisions.
- The API exposes enough callback context to implement projection and mutation
  without downstream code replaying token events from a private parser state.
- The common pass-through path should be straight-line and cheap. A design that
  requires downstream consumers to perform per-token cache lookups,
  selector-result invalidation checks, or replay bookkeeping in the hot path
  does not satisfy the performance intent.
- Candidate metadata remains available with the existing meanings: candidate
  index, stream offset, candidate byte size, payload size when applicable, and
  callback ordering.
- Stop and error propagation follow the existing candidate visitor rules.
- Fragmented reader input produces the same logical transform output as
  unfragmented input.
- Memory remains bounded by parser stack, writer state, configured chunk/current
  value buffers, and caller-owned selector/projection/mutation scratch. The API
  must not require retaining the whole source, the whole candidate stream, all
  candidates, or all matched outputs.
- Existing candidate capture modes keep their current behavior. This is an
  additional public surface, not a semantic change to `CAPTURE_SPOOLED`.

## Projection Requirements

liblql projection needs to be able to:

- inspect path-aware events for every value in a candidate;
- decide whether the current value contributes to the projected output;
- emit only projected object members or array elements through lonejson writer
  validation;
- preserve malformed JSON, callback failure, and stop behavior already exposed
  by candidate visitors;
- avoid retaining a complete candidate or complete projection result in liblql.

The projected output does not need to be byte-identical to Go or to the input.
It must be logically valid JSON matching the projection semantics.

## Mutation Requirements

liblql mutation needs to be able to:

- observe candidate selector state while the candidate is parsed;
- pass through unmatched candidates according to caller policy;
- suppress removed object members or array elements;
- replace existing values with caller-provided JSON/string/number/boolean/null
  values;
- insert created object members in positions supported by the transform model,
  or report unsupported insert positions clearly when the model cannot express
  them in one pass;
- keep writer validation and escaping inside lonejson;
- avoid retaining a complete candidate or complete mutation result in liblql.

The API should make unsupported one-pass mutation shapes explicit rather than
encouraging downstream libraries to fall back to hidden materialization.

## Non-Goals

- Do not add LQL selector, projection, or mutation semantics to lonejson.
- Do not expose private parser internals as a substitute public API.
- Do not require downstream libraries to implement their own JSON tokenizer,
  serializer, compact writer, or candidate framer.
- Do not add a liblql-specific callback hook.
- Do not make this depend on seekable input.
- Do not implement this with full-candidate memory capture, temporary files as
  hidden staging, or full-output buffering.
- Do not use selector-result, candidate-result, or transform-output caching to
  hide the replay cost.
- Do not change `AUTO`, `NDJSON`, `SINGLE_VALUE`, `ARRAY_ITEMS`, or existing
  capture mode semantics.

## Validation Required In LoneJSON

The lonejson test suite should prove at least:

- dense-match projection transforms a candidate stream without candidate
  spooling or replay;
- dense-match mutation transforms matched object candidates and passes through
  or suppresses unmatched candidates according to caller policy;
- sparse selectors can compose with any predicate-gated retain/discard feature
  without changing candidate metadata;
- fragmented reader input across every relevant token boundary produces the
  same logical output as unfragmented input;
- stop from observer or transform callbacks prevents later candidates from
  being emitted;
- callback failure reports through the existing candidate-stream error surface;
- malformed JSON after complete candidates preserves prior callbacks and reports
  the parse error;
- peak memory is independent of total source size and candidate count;
- pass-through output remains valid JSON and does not require full-candidate
  capture;
- replacement output is validated/escaped by lonejson, not by downstream string
  concatenation.

## liblql Acceptance Criteria After LoneJSON Support

Once this surface exists, liblql should replace callback-source projection and
candidate mutation replay paths with true single-pass source transforms.

The liblql acceptance gate should prove:

- selector parity remains unchanged for transformed and untransformed streams;
- projection and mutation outputs preserve existing public semantics;
- callback-source dense/all-match transforms no longer call into candidate
  spooled capture for every candidate;
- no liblql-owned full candidate or output materialization is introduced;
- warmed receiver allocation tests still pass;
- `make bench-check`, `make bench-memory-check`, and the 1 GiB memory gate
  remain green;
- release profiles no longer show dense source projection/mutation dominated by
  `lonejson_spooled_append` and candidate replay work.
