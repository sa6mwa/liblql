# LoneJSON CR: Streaming Candidate Processing Primitives For Query Engines

## Status

This is the single handoff CR for the lonejson changes liblql needs after
`lonejson` `v0.36.0`. It supersedes the narrower liblql-local CR notes for:

- predicate-gated candidate capture;
- single-pass candidate transforms;
- chunked JSON number writer output.

Those three items are one coherent dependency request because they address the
same boundary: a downstream query engine must be able to observe, select,
project, mutate, and emit JSON candidates through lonejson without adding a
second parser, a private serializer, hidden full-candidate materialization, or
spool/replay work in the hot path.

This CR is intentionally about generic JSON streaming primitives. It is not a
request for lonejson to implement LQL.

## Executive Summary

liblql uses lonejson as the owner of JSON parsing, validation, candidate
framing, serialization, escaping, writer state, and stream normalization. That
ownership is the correct architecture and should not be weakened.

The current public lonejson surfaces are sufficient for correctness, including
non-seekable callback sources, because liblql can fall back to bounded
candidate spooling and replay. They are not sufficient for the final
performance and streaming shape of a C-native query engine, because several
important operations still require all-candidate capture, candidate replay, a
second parse, or downstream buffering of complete JSON number tokens.

The requested change is to add public lonejson APIs that let a caller:

1. observe a candidate while it is parsed, then retain only selected candidate
   payloads after selector truth is known;
2. observe and transform a candidate from the same validated token stream,
   without replaying the candidate through another parse;
3. pass through large JSON number tokens to a lonejson writer in chunks,
   without downstream allocation of the complete number text.

If implemented correctly, liblql keeps all LQL selector, AST, projection, and
mutation semantics in liblql, while lonejson keeps all JSON parsing and writing
responsibilities in lonejson.

## Background And Problem Statement

LQL is a query and mutation language over JSON. The C port, liblql, exposes an
AST-centered selector API and a Lua facade, and it uses lonejson for JSON input
and output. The liblql implementation must handle very large JSON sources on
small systems. It must not require memory proportional to source size, candidate
count, match count, or output size.

Seekable inputs are already efficient for many payload cases. When the source
is a file, fd, mmap-like source, or any other source where the original bytes
can be reread by offset, lonejson's 64-bit candidate offsets and sizes let
liblql re-open the exact candidate byte range without asking lonejson to retain
that candidate while parsing the stream.

Non-seekable callback sources are different. Once bytes pass through the
parser, liblql cannot rewind them. Today, when liblql might need a selected
candidate payload later, it must request candidate capture before it knows
whether the candidate matches. For sparse selectors, this means unmatched
candidates are still captured and then discarded. For dense projection or
mutation, this means every candidate is captured, replayed, and reparsed so the
transform writer can produce output.

That fallback is correct and bounded when documented as spooled/captured
behavior. It is not the final desired streaming path:

- sparse callback-source matches pay capture cost for candidates that are known
  to be irrelevant only at candidate end;
- dense callback-source projection and mutation pay capture, replay, and second
  parse cost for every candidate;
- pass-through number tokens require downstream buffering because the writer
  accepts only a complete number token, even though parser visitors expose the
  token incrementally.

The missing features belong in lonejson because they are JSON parser/writer
composition primitives. If liblql implements them privately, liblql must either
duplicate JSON framing/serialization behavior or depend on private parser state.
Both outcomes are wrong.

## Design Principles

These principles are binding for this CR:

- LoneJSON owns JSON correctness: parsing, validation, candidate framing,
  escaping, separators, number-token validation, writer state, and source/sink
  error propagation remain in lonejson.
- Downstream libraries own domain semantics: selectors, ASTs, query language,
  projection rules, mutation rules, limits, and application callbacks remain
  outside lonejson.
- Streaming must be real streaming. Bounded parser buffers, writer buffers,
  source chunks, sink chunks, and current-token state are acceptable. Hidden
  full-source buffering, hidden full-candidate buffering, hidden full-output
  buffering, or temp files presented as streaming are not acceptable.
- Spooled or captured behavior is acceptable only when the API name and
  documentation say it is spooled or captured, and only for the specific current
  candidate or payload the caller explicitly requested.
- Memory use must not grow with total source size, total candidate count, total
  match count, or total output size.
- Existing public lonejson behavior must remain compatible. The requested
  surfaces are additive unless lonejson intentionally designs a better
  compatible evolution path.
- APIs must work with fragmented callback readers. Correctness must not depend
  on token, string, number, object, array, or candidate boundaries lining up
  with read boundaries.
- Candidate metadata must retain existing meanings: candidate index, stream
  offset, candidate byte size, payload size when a payload exists, callback
  ordering, stop behavior, and error behavior.
- Stop and failure must be explicit. Callback stop, callback error, source read
  error, sink write error, parse error, writer error, and validation error must
  be distinguishable through existing or clearly documented lonejson diagnostic
  surfaces.

## Required Feature 1: Predicate-Gated Candidate Capture

### Intent

Add a public candidate-stream capture mode, callback, or equivalent API that
lets a downstream observer decide at candidate end whether the just-validated
candidate payload should be retained.

This exists for non-seekable sources where selector truth is known only after
the candidate has been parsed. It avoids retaining unmatched candidates while
still letting matched candidates expose a callback-scoped replayable payload.

### Required Behavior

- LoneJSON validates and frames each candidate exactly once.
- Path-aware visitor callbacks run while candidate bytes are consumed, so the
  caller can evaluate selector state during the parse.
- At candidate end, after all path-aware state for that candidate is final and
  before any replayable payload is exposed to the ordinary candidate-end
  callback, lonejson asks the caller whether to retain or discard the current
  candidate.
- If the caller says retain, the candidate-end callback receives a replayable
  payload handle with the same lifetime and replay semantics as existing public
  spooled capture.
- If the caller says discard, the candidate-end callback must not receive a
  replayable payload handle for that candidate, and lonejson must release any
  dependency-owned replay state for that candidate before parsing the next
  candidate.
- A discard decision must not require the caller to consume, drain, or free
  partially captured bytes.
- A retain decision must respect existing configured memory/spool policies,
  maximum payload sizes, and failure behavior.
- Callback stop from the retain/discard decision must prevent later candidates
  from being parsed and must report as a normal stop, not as malformed JSON.
- Callback error from the retain/discard decision must report through the same
  diagnostic channel as other candidate visitor callback errors.
- Reader, buffer, file, path, and fd candidate-stream entry points should be
  supported where those entry points already support candidate capture.
- `AUTO`, `SINGLE_VALUE`, `NDJSON`, and `ARRAY_ITEMS` framing semantics must
  not change.
- Nested arrays that become candidates under existing candidate-stream framing
  must preserve source-relative index and byte offsets. Re-entering a nested
  array or internally delegating to another candidate parser must not reset
  publicly reported coordinates.

### API Shape Guidance

The exact API names are a lonejson design choice. The semantics above are the
contract.

One acceptable shape would be an additional capture mode such as
`LONEJSON_CANDIDATE_CAPTURE_GATED_SPOOLED` plus a decision callback returning
`retain`, `discard`, `stop`, or `error`. Another acceptable shape would be an
options callback used by an existing capture mode. In either design, the
callback timing and ownership rules must be unambiguous.

The API should avoid exposing parser internals. The caller should receive
candidate metadata and its own callback context, not a mutable parser frame or
private capture buffer.

### What This Feature Must Not Do

- It must not add selector, query, projection, or mutation semantics to
  lonejson.
- It must not retain a full source or an array of candidate payloads.
- It must not hide temp-file staging behind a name that appears to be pure
  streaming.
- It must not require downstream code to reconstruct a candidate from token
  callbacks.
- It must not change existing capture modes or make current spooled capture
  lazy in a way that changes observable behavior.
- It must not depend on byte-identical serialization. Retained payload replay
  only needs to satisfy the existing lonejson replay contract.

### LoneJSON Validation Required

The lonejson test suite should prove:

- sparse match fixtures discard unmatched candidates without exposing payload
  handles;
- dense match fixtures retain all candidates and replay them with the same
  logical behavior as existing spooled capture;
- arbitrary reader fragmentation preserves retain/discard behavior;
- stop and error from the decision callback behave like other visitor stop and
  error paths;
- discarded candidates do not leak files, allocations, payload handles, or
  other replay state;
- retained candidates obey configured spool and size policies;
- candidate index, stream offset, and byte size are identical to existing
  capture modes for the same input and framing;
- nested top-level array candidates report source-relative coordinates, not
  coordinates relative to an internal nested spool or delegated parser.

## Required Feature 2: Single-Pass Candidate Transform

### Intent

Add a public candidate-stream transform API, or equivalent public visitor
composition API, that lets one candidate parse drive both selector observation
and output transformation.

This exists so downstream query engines can project or mutate non-seekable
candidate streams without capturing each candidate, replaying it, and parsing
it again.

### Required Behavior

- LoneJSON validates and frames each candidate exactly once.
- The same token stream can drive:
  - a path-aware observer used by the caller to evaluate selector state;
  - a transform writer used by the caller to pass through, suppress, or replace
    values while emitting valid JSON.
- The transform writer must remain lonejson-owned. Object/array structure,
  separators, object key/value state, string escaping, number validation,
  top-level value rules, sink writes, and error reporting are not delegated to
  downstream string concatenation.
- The caller must be able to pass through the current value without rebuilding
  that value manually.
- The caller must be able to suppress the current value when suppression is
  legal for the current container position.
- The caller must be able to replace the current value with caller-provided
  JSON, string, number, boolean, or null output through validated writer
  surfaces.
- The API must expose enough context for projection and mutation decisions:
  candidate metadata, path information, container kind, object member name when
  applicable, array index when applicable, scalar type, and current callback
  phase.
- The common pass-through path must be cheap. It should not require
  downstream-maintained per-token caches, selector-result replay queues, or
  invalidation maps in the hot path.
- Output framing must be explicit. If lonejson emits a candidate stream, it
  must own the separators/framing for that stream. If lonejson emits each
  transformed candidate to a caller-provided sink, the API must state exactly
  who owns separators between candidates and how invalid mixed stream shapes are
  rejected.
- Fragmented reader input must produce the same logical output as unfragmented
  input.
- Stop and error from either the observer side or transform side must stop
  later output and report through documented diagnostics.
- The transform must not require retaining the whole source, whole candidate
  stream, all candidates, all matches, or full output.

### Projection Semantics Required By liblql

The API must be expressive enough for liblql projection to:

- observe every value in a candidate through path-aware callbacks;
- decide whether a value contributes to the projected output;
- emit only selected object members or array elements;
- preserve valid JSON output when intermediate object members or array elements
  are suppressed;
- preserve malformed JSON, callback stop, callback error, and sink error
  behavior already exposed by candidate visitors and writers;
- avoid retaining the complete input candidate or complete projected result.

The output does not need to be byte-identical to Go LQL or to the source input.
It must be logically valid JSON matching the projection semantics.

### Mutation Semantics Required By liblql

The API must be expressive enough for liblql mutation to:

- observe selector state while each candidate is parsed;
- pass through unmatched candidates according to caller policy;
- suppress removed object members or array elements;
- replace existing values with caller-provided JSON, string, number, boolean,
  or null output;
- insert created object members only when the one-pass transform model can
  express the insertion without hidden materialization;
- clearly report unsupported one-pass insertion positions or mutation shapes
  rather than silently falling back to full-candidate buffering;
- keep all writer validation and escaping inside lonejson;
- avoid retaining the complete input candidate or complete mutation result.

One-pass mutation cannot always express every possible edit at every possible
position without knowing future input. When a shape is not representable in a
true streaming transform, the API should fail with a clear unsupported-shape
status or diagnostic. It should not pretend to stream while materializing the
candidate in private.

### API Shape Guidance

The exact callback names are a lonejson design choice. The required design is a
composition of parser observation and writer control, not a liblql-specific
hook.

An acceptable shape could expose a candidate transform visitor where callbacks
receive path context and a writer/control handle for the current value. The
control handle could support operations such as pass-through current value,
suppress current value, replace with writer-produced value, stop, or error.

Another acceptable shape could expose a more general token-pipeline
composition API, as long as downstream code does not have to implement its own
JSON tokenizer, serializer, candidate framer, or replay buffer.

The API must document callback ordering precisely enough that a downstream
query engine can know when selector state for the current value, containing
object member, containing array element, and complete candidate is final.

### What This Feature Must Not Do

- It must not add LQL selector, AST, projection, or mutation rules to
  lonejson.
- It must not expose private parser state as the public API.
- It must not require downstream code to emit raw JSON punctuation, separators,
  string escapes, or number text around lonejson writer state.
- It must not require a seekable input source.
- It must not hide full-candidate capture, hidden temp files, or full-output
  buffering behind a transform API.
- It must not solve performance by caching selector or transform results.
  Caches add memory growth and hot-path branching without addressing the core
  missing primitive.
- It must not change existing candidate framing or capture semantics.

### LoneJSON Validation Required

The lonejson test suite should prove:

- dense projection transforms a candidate stream without candidate spooling or
  replay;
- dense mutation transforms matched candidates and passes through or suppresses
  unmatched candidates according to caller policy;
- sparse selectors compose with predicate-gated capture without changing
  candidate metadata;
- arbitrary fragmentation across strings, numbers, object names, object
  values, arrays, and candidate boundaries produces the same logical output as
  unfragmented input;
- observer stop, transform stop, observer error, transform error, parse error,
  and sink error each report through the documented status/diagnostic path;
- malformed JSON after complete prior candidates preserves prior callbacks and
  reports the later parse error;
- peak memory remains independent of total source size, candidate count, match
  count, and output size;
- pass-through output is valid JSON without full-candidate capture;
- replacement output is validated and escaped by lonejson.

## Required Feature 3: Chunked JSON Number Writer

### Intent

Add public writer entry points, or an equivalent writer mode, that let a caller
emit one JSON number token in chunks.

This exists because lonejson visitors already expose numbers as begin/chunk/end
events, while the writer currently accepts number text as one complete token.
For pass-through projection and mutation, downstream code should not have to
allocate the complete number text simply to hand it back to lonejson.

### Required Behavior

- A number-begin operation opens one JSON number value at a writer position
  where a scalar value is legal.
- One or more number-chunk operations append fragments of that token.
- A number-end operation closes the token, validates the complete JSON number,
  and commits it to output if valid.
- Empty number tokens must be rejected.
- Invalid number tokens must report a writer error and must not silently emit
  invalid JSON.
- Writer structure rules remain lonejson-owned: object keys, object values,
  array elements, top-level values, separators, and error state must match
  existing writer behavior.
- Splitting a valid number across arbitrary chunks must produce the same
  logical output as the existing complete-number writer API.
- Memory must be bounded by current-token validation state and writer buffers.
  It must not require downstream code to retain a complete source value, whole
  candidate, or full output value.
- Existing complete-token number APIs must remain unchanged.

An implementation may internally buffer the current number token if that is the
right way for lonejson to validate numbers. The important requirement is that
the buffering is lonejson-owned, current-token scoped, and not pushed into
downstream query-engine code.

### What This Feature Must Not Do

- It must not expose a raw JSON write escape hatch.
- It must not allow arbitrary punctuation, object separators, array separators,
  or raw object keys to bypass writer state.
- It must not require downstream code to pre-validate number syntax.
- It must not change number formatting semantics beyond preserving the accepted
  token text according to existing writer validation.
- It must not attempt to solve candidate replay or selector-gated capture.

### LoneJSON Validation Required

The lonejson test suite should prove:

- one-chunk and multi-chunk valid numbers match complete-token writer output;
- chunks split across sign, integer, decimal point, fractional digits, exponent
  marker, exponent sign, and exponent digits are accepted when the whole token
  is valid;
- invalid JSON numbers fail at number-end with an actionable writer diagnostic;
- chunked numbers work as object member values, array elements, and top-level
  values;
- calls in invalid writer states fail consistently with existing writer error
  rules;
- writer state after a number validation failure is documented and tested;
- existing number writer APIs and JSON value writer APIs remain unchanged.

## Composition Requirements

The three requested features must compose cleanly:

- Predicate-gated capture and single-pass transform must be usable in the same
  candidate-stream architecture without conflicting callback ordering.
- A sparse query should be able to observe candidates, discard unmatched
  candidates without payload exposure, and retain only matched payloads.
- A dense transform should be able to observe and emit each candidate without
  spooling or replaying every candidate.
- A transform should be able to pass through large number tokens with the
  chunked number writer path, without downstream allocation of complete number
  text.
- Candidate metadata must remain stable regardless of whether a candidate is
  retained, discarded, transformed, suppressed, or passed through.
- LQL-style query limits are not lonejson's responsibility, but lonejson stop
  callbacks must be strong enough for liblql to enforce those limits without
  parsing or emitting later candidates.
- Seekable-source offset workflows remain valid and should not be forced
  through capture. If the source can be reread by offset, liblql should still
  be able to use offsets instead of candidate capture.

## Explicit Non-Goals For The Whole CR

This CR does not ask lonejson to implement:

- LQL selector syntax;
- LQL selector AST types;
- LQL evaluation semantics;
- projection or mutation semantics specific to LQL;
- callback query limits;
- application-level match decisions;
- pretty or colorized JSON;
- byte-identical output compared with Go LQL or any source file;
- OAuth, OIDC, network authentication, or transport behavior;
- caching of selectors, candidates, token streams, transform decisions, or
  output values;
- hidden materialization as a substitute for streaming;
- a public raw-JSON writer escape hatch;
- a second public parser facade for downstream libraries to stitch together
  manually.

If an operation cannot be represented through the proposed streaming surfaces,
lonejson should return a clear unsupported-shape or invalid-use status. It
should not silently materialize a candidate or output value to make the API
appear more capable than it is.

## Required Documentation In LoneJSON

The lonejson documentation for these features should state:

- which APIs are streaming, which are spooled/captured, and which are
  current-token buffered;
- callback ordering for path observation, retain/discard decisions, transform
  decisions, candidate-end callbacks, and sink writes;
- ownership and lifetime of replayable payload handles;
- ownership and lifetime of writer/control handles passed to transform
  callbacks;
- source-relative meaning of candidate index and byte offsets;
- behavior on fragmented input;
- behavior on callback stop and callback error;
- behavior on source read failure and sink write failure;
- whether output framing is lonejson-owned or caller-owned for each transform
  entry point;
- unsupported transform shapes and the status/diagnostic used for them.

## liblql Acceptance Criteria After LoneJSON Support

After the lonejson features are available, liblql should be able to replace its
current fallback paths with these primitives. The liblql acceptance gate should
prove:

- selector, projection, mutation, payload, CLI, Lua facade, and JSON AST parity
  remain unchanged for supported public behavior;
- callback-source sparse plus-value and matches-only mutation do not capture
  discarded candidates;
- callback-source dense projection and mutation do not capture, replay, and
  reparse every candidate;
- pass-through long number tokens do not allocate liblql receiver memory merely
  to hand the token back to lonejson;
- no liblql-owned JSON parser, tokenizer, serializer, escaper, candidate
  framer, or fixture normalizer is introduced;
- no hidden full-source, full-candidate-stream, all-candidate, all-match, or
  full-output materialization is introduced;
- warmed receiver allocation tests, sanitizer tests, parity gates, memory
  gates, and benchmark gates remain green;
- performance profiles for sparse callback-source payload paths and dense
  callback-source transform paths no longer show all-candidate
  `lonejson_spooled_append`, replay, or downstream number buffering as dominant
  hot-path costs.

## Suggested Implementation Order

The features can be implemented independently, but this order gives the most
useful incremental value to liblql:

1. Predicate-gated candidate capture, because it removes wasted capture for
   sparse non-seekable payload and matches-only paths without requiring a new
   transform pipeline.
2. Single-pass candidate transform, because it removes dense non-seekable
   projection and mutation replay.
3. Chunked JSON number writer, because it removes the remaining downstream
   allocation for straight pass-through number tokens and composes naturally
   with the transform API.

This ordering is not a semantic requirement. The final public behavior is the
important part.

## Definition Of Done

The CR is complete in lonejson when:

- public headers expose stable APIs for all three feature groups;
- tests cover the required success, fragmentation, stop, error, metadata,
  memory, and compatibility cases listed above;
- documentation names streaming, captured, spooled, and current-token-buffered
  behavior precisely;
- existing public candidate-stream and writer APIs remain compatible;
- no new API requires downstream code to parse, serialize, frame, escape, or
  raw-write JSON outside lonejson;
- liblql can remove the corresponding fallback capture/replay/number-buffering
  hot paths while preserving its public behavior.
