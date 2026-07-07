# LoneJSON CR: Candidate Transform V2 For liblql Full Switch-Over

## Status

This CR is a follow-up to the `lonejson v0.37.0` candidate transform API.

`v0.37.0` added useful public APIs:

- `lonejson_transform_candidates_*`;
- `lonejson_candidate_transform_options`;
- `lonejson_candidate_transform_fn`;
- `lonejson_candidate_transform_replace_fn`;
- `LONEJSON_CANDIDATE_TRANSFORM_KEEP`;
- `LONEJSON_CANDIDATE_TRANSFORM_DROP`;
- `LONEJSON_CANDIDATE_TRANSFORM_REPLACE`;
- `LONEJSON_CANDIDATE_TRANSFORM_STOP`;
- `LONEJSON_CANDIDATE_TRANSFORM_ERROR`.

That surface is not sufficient for liblql to replace its current
callback-source candidate projection and mutation replay paths without changing
public behavior or adding downstream materialization.

This CR defines the complete public lonejson behavior needed for liblql to
perform that switch-over.

## Intent

liblql needs a lonejson-owned candidate transform surface that can:

1. parse and validate each input candidate once;
2. expose the original token stream to liblql selector/projection/mutation
   observers;
3. let liblql keep, drop, replace, or synthesize JSON values through lonejson's
   writer;
4. preserve liblql's existing candidate-stream semantics, including nested
   root-array flattening, per-emitted-candidate newlines, query limits, and
   partial-result accounting;
5. avoid full-source, full-candidate, all-candidate, all-match, and full-output
   materialization.

This is not a request for lonejson to implement LQL. liblql keeps selector,
projection, mutation, limit, and callback semantics. lonejson keeps JSON
tokenization, validation, candidate framing, writer state, escaping, separators,
and sink/source error propagation.

## Why v0.37.0 Is Not Enough

The `v0.37.0` transform event contains:

```c
typedef struct lonejson_candidate_transform_event {
  const lonejson_candidate_info *candidate;
  const lonejson_value_path *path;
  lonejson_value_type value_type;
} lonejson_candidate_transform_event;
```

The replacement callback receives that event plus a `lonejson_writer *`.

That leaves these gaps for liblql:

- The transform event does not expose the old scalar value for the current
  value. liblql numeric increment requires the current number text or equivalent
  numeric value at the selected path before it can write the replacement.
- The API does not specify observer/transform ordering strongly enough for
  liblql to know that an observer has already received the complete current
  scalar before `transform` or `replace` is called.
- The output framing mode emits newline separators between candidates, but does
  not emit liblql's existing newline after every emitted candidate. liblql needs
  explicit control of final candidate termination.
- Root-array candidate handling is not enough for liblql. liblql's current
  candidate streams recursively flatten root-array candidates as candidate
  streams. Keeping a nested root array as `[ ... ]` changes public behavior.
- Candidate-level decisions are not sufficient for matches-only mutation and
  projection unless selector truth is final before the output decision for the
  candidate root is committed.
- Projection needs structural synthesis: selected deep fields require emitting
  parent objects/arrays while suppressing unrelated siblings. A value-level
  keep/drop/replace callback is not enough unless lonejson exposes a way to
  synthesize these containers through the same writer contract without raw JSON
  punctuation or full-output buffering.
- Created mutation members require a defined insertion model. liblql must know
  when it can add object members in one pass and when lonejson will report an
  unsupported transform shape.

## Required API Semantics

The exact names are a lonejson design choice. The public semantics below are
required.

### Candidate Stream Input

The API must support the same source families as candidate streaming:

- caller buffer;
- caller reader callback;
- `FILE *`;
- filesystem path;
- file descriptor.

It must support the same candidate framing modes that liblql currently relies
on:

- repeated top-level JSON values;
- NDJSON;
- single JSON value;
- top-level array items.

The transform must parse each input byte through lonejson exactly once for a
non-seekable reader. It must not implement transform by privately spooling a
candidate and then replaying it through another parser.

### Candidate Metadata

Every candidate-level callback and value-level transform event must expose
candidate metadata with the existing meanings:

- candidate index;
- source-relative stream offset;
- source-relative candidate byte size;
- payload size when a payload exists;
- framing mode behavior;
- stop/error status.

If a root array is expanded into nested candidates, emitted nested candidates
must have stable source-relative metadata. Internal delegated parsing must not
reset public coordinates to a temporary spool or nested parser origin.

### Observer Ordering

The API must document and guarantee callback ordering for all of these phases:

1. candidate begin;
2. original token/value observation;
3. value transform decision;
4. replacement emission;
5. candidate root output decision;
6. candidate end;
7. stop/error propagation.

For every value, liblql must be able to know whether the original value has
already been completely observed when a transform decision is requested.

For scalar values, the required ordering is:

- string begin/chunk/end observation completes before any replacement callback
  that needs the complete old string;
- number begin/chunk/end observation completes before any replacement callback
  that needs the complete old number;
- boolean/null observation completes before any replacement callback that needs
  the old scalar.

If lonejson chooses not to guarantee observer-before-transform ordering, it must
instead pass the current old value directly to the transform/replacement
callback as described below.

### Old Value Access

The transform API must support old-value-dependent replacement without
candidate materialization.

For the current value selected by a transform callback, the callback must be
able to inspect:

- value type;
- path;
- object member key when the value is an object member;
- array index when the value is an array element;
- for strings: decoded UTF-8 chunks or a callback-scoped streaming view;
- for numbers: validated raw JSON number chunks or a callback-scoped streaming
  view;
- for booleans: the boolean value;
- for null: null type;
- for objects/arrays: begin/end observation and child value observation.

For liblql numeric increment, the minimum required support is one of:

- the replacement callback receives the current number text as chunks before it
  must write the replacement;
- the replacement callback receives a callback-scoped old-value visitor/source
  that can stream the current number exactly once;
- lonejson guarantees that the transform observer has already delivered the full
  number token to caller state before replacement is requested.

This must not require liblql to retain a whole candidate. Current-scalar
scratch is acceptable where the LQL operation itself requires it, such as numeric
increment. Straight pass-through numbers must remain pass-through through
lonejson and must not require liblql-owned complete-token buffering.

### Transform Controls

For each transformable value, lonejson must allow the caller to choose:

- keep current value unchanged;
- drop current value when legal;
- replace current value by writing one valid JSON value through a lonejson
  writer;
- stop successfully before later candidates are parsed or emitted;
- fail with a diagnostic.

Drop legality must be documented:

- dropping an object member drops both key and value;
- dropping an array element omits the element and preserves valid array
  separators;
- dropping a candidate root suppresses that candidate's output;
- dropping a root value in a single-value transform is either supported with
  documented empty-output behavior or rejected with a clear unsupported status.

Replacement must be writer-owned. Callers must not need to write raw JSON
punctuation, commas, colons, string escapes, object keys, or array separators.

### Object Member Insertion

liblql mutation creates missing object members for supported plans. The
candidate transform API must define how a caller can insert object members in a
single pass.

At minimum, lonejson must support one of these models:

1. **Object-boundary insertion hooks**
   - on object begin: caller may request members to be emitted before source
     members;
   - before/after each source member: caller may request inserted members;
   - on object end: caller may request members to be emitted after all source
     members.

2. **Object builder callback**
   - lonejson owns object framing;
   - caller can emit zero or more object members through a writer/control handle;
   - lonejson then continues source member pass-through/drop/replace decisions.

3. **Explicit unsupported-shape reporting**
   - if lonejson deliberately does not support insertion in a particular one-pass
     context, it returns a documented unsupported status before producing partial
     invalid output.

The API must never solve insertion by privately materializing the full object or
candidate while presenting the result as streaming.

### Projection Synthesis

liblql projection is not only value replacement. It selects fields and emits a
new JSON value containing the required parent containers.

The API must let liblql implement projection without raw writer bypass or full
projected-output buffering. It must support one of:

- a transform mode where the caller owns projected output construction through a
  lonejson writer while lonejson still owns candidate framing and sink writes;
- object/array boundary hooks that let the caller open/close synthesized
  containers and emit selected descendants while lonejson manages writer state;
- a documented general writer-control handle that can suppress source subtrees
  and emit caller-constructed replacement subtrees.

The projection path must be able to:

- observe every original source value for selector/projection decisions;
- suppress unselected siblings;
- emit selected deep descendants under synthesized parent objects/arrays;
- emit arrays with `null` placeholders where liblql projection semantics require
  them;
- report missing projection as no output for that candidate;
- avoid retaining the complete candidate or complete projected result.

### Candidate Root Decisions

liblql needs decisions at the candidate-root level after selector truth is final:

- preserve unmatched candidates when `matches_only == false`;
- drop unmatched candidates when `matches_only == true`;
- mutate matched candidates;
- project matched candidates;
- project then mutate matched candidates;
- stop before later candidates when query limits are reached.

The transform API must allow root output decision after the candidate has been
fully observed but before output for that candidate is irreversibly committed,
or it must provide an equivalent streaming control model that preserves the same
semantics without full-candidate buffering.

If lonejson writes pass-through output incrementally before candidate-end
selector truth is known, it cannot support sparse `matches_only` or projection
root decisions without additional buffering. That behavior must therefore be
explicitly named as unsupported for those modes, not silently used by liblql.

### Nested Root-Array Flattening

liblql candidate streams recursively flatten root-array candidates. For example:

```json
[{"id":"a"}, [{"id":"b"}], {"id":"c"}]
```

is treated as candidate outputs:

```json
{"id":"a"}
{"id":"b"}
{"id":"c"}
```

The transform API must support this behavior for transform paths that claim
candidate-stream compatibility with liblql.

Required behavior:

- a root array under candidate-stream framing can be expanded into item
  candidates;
- nested root arrays can be expanded recursively;
- selector observation and mutation/projection decisions apply to the emitted
  item candidates, not to the wrapper array value;
- source-relative candidate metadata is preserved;
- query limits can stop during nested expansion without parsing later nested or
  outer candidates;
- partial results before source read failure or parse failure remain reportable.

If lonejson chooses to keep nested arrays as ordinary values in a transform API,
that API is not sufficient for liblql candidate mutation/projection switch-over.

### Output Framing

liblql currently emits one newline after every emitted candidate in candidate
mutation/projection streams.

The transform API must allow this exact framing:

- no output for dropped candidates;
- each emitted candidate is one complete JSON value;
- each emitted candidate is followed by `\n`, including the last emitted
  candidate;
- no extra newline is emitted when no candidates are emitted;
- sink failures while writing candidate payload or newline report as transform
  sink/write errors.

An NDJSON mode that only emits separators between candidates is not sufficient
unless it also exposes an option for trailing newline after each emitted
candidate.

### Stop And Error Semantics

The API must distinguish:

- successful completion;
- successful callback stop;
- observer callback error;
- transform callback error;
- replacement writer error;
- source read error;
- sink write error;
- malformed JSON;
- unsupported transform shape.

When an error occurs after complete prior candidates were emitted, those prior
candidates remain emitted and liblql can report partial result counters.

When stop is requested because of query limits or caller callback stop, later
input candidates must not be parsed and later output candidates must not be
emitted.

### Memory Contract

The API must be real streaming.

Allowed memory:

- parser stack;
- writer state;
- source and sink chunks;
- current token validation state;
- current scalar scratch when explicitly required by the caller;
- caller-owned selector/projection/mutation state;
- current object/array transform state required for legal separator handling.

Forbidden memory:

- full source buffering;
- full candidate buffering;
- all-candidate buffering;
- all-match buffering;
- full output buffering;
- temp files used as hidden substitute for streaming;
- replay queues of token events used to hide a second parse.

If an implementation internally buffers a current scalar token, that behavior
must be documented as current-token scoped. It must not grow with candidate size
except for the current scalar itself, and it must not force downstream callers to
buffer pass-through scalar tokens.

## Required liblql Coverage Cases

The lonejson API must be strong enough for liblql to implement these existing
public behaviors without candidate replay.

### Candidate Mutation

Input:

```json
{"id":"a","status":"open"}
{"id":"b","status":"closed"}
```

Selector: `/status="open"`

Mutation: `/status=done`

`matches_only=false` output:

```json
{"id":"a","status":"done"}
{"id":"b","status":"closed"}
```

`matches_only=true` output:

```json
{"id":"a","status":"done"}
```

Each emitted candidate has a trailing newline.

### Match-All Mixed Candidate Mutation

Input:

```json
[{"id":"a","status":"open"}, 7, [{"id":"b","status":"open"}], true]
```

Match-all mutation: `/status=done`

Output:

```json
{"id":"a","status":"done"}
7
{"id":"b","status":"done"}
true
```

Each emitted candidate has a trailing newline. The nested array is flattened.

### Numeric Increment

Input:

```json
{"id":"a","count":41}
```

Mutation: `/count++`

Output:

```json
{"id":"a","count":42}
```

The replacement callback must have access to the old number value/token without
candidate replay.

### Remove

Input:

```json
{"id":"a","old":true,"keep":1}
```

Mutation: `rm:/old`

Output:

```json
{"id":"a","keep":1}
```

The object key and value are suppressed together. Separators remain valid.

### Create Missing Member

Input:

```json
{"id":"a"}
```

Mutation: `/status=done`

Output:

```json
{"id":"a","status":"done"}
```

The API must either support this insertion in one pass or report unsupported
before partial invalid output.

### Projection

Input:

```json
{"id":"a","meta":{"trace":9,"drop":1},"items":[{"sku":"A"},{"sku":"B"}]}
```

Projection paths:

- `/id`;
- `/meta/trace`;
- `/items/1/sku`.

Output:

```json
{"id":"a","meta":{"trace":9},"items":[null,{"sku":"B"}]}
```

The API must allow liblql to synthesize parent containers and array placeholders
without full-result buffering.

### Project Then Mutate

Input:

```json
{"id":"a","state":{"count":1,"drop":true}}
{"id":"b","state":{"count":1,"drop":true}}
```

Selector: `/id="a"`

Projection paths:

- `/id`;
- `/state/count`.

Mutation: `/state/count++`

`matches_only=false` output:

```json
{"id":"a","state":{"count":2}}
{"id":"b","state":{"count":1}}
```

The unmatched candidate is projected but not mutated. The matched candidate is
projected and mutated. No candidate is replayed.

### Limits And Partial Results

For `max_matches=1`, the transform must stop after the first matched emitted
candidate and must not parse or emit later candidates.

For a source read failure after one complete candidate, lonejson must return a
source read error while preserving the first emitted candidate and allowing
liblql to report `candidates_seen == 1`.

## LoneJSON Test Requirements

The lonejson test suite must prove:

- candidate transform does not use candidate capture/spooling internally for
  reader input;
- observer-before-transform ordering or old-value callback delivery is
  deterministic and documented;
- numeric old-value replacement works for numbers split across arbitrary reader
  chunk boundaries;
- string old-value observation works for strings split across arbitrary reader
  chunk boundaries;
- root candidate drop suppresses output and does not emit blank lines;
- object member drop removes key and value and preserves valid separators;
- array element drop preserves valid separators;
- replacement writer validates and escapes strings;
- replacement writer validates numbers;
- object member insertion works in every documented supported position;
- unsupported insertion shapes fail with a documented unsupported status and no
  partial invalid output;
- projection-style suppression plus synthesized output can be expressed without
  raw JSON punctuation from the caller;
- nested root-array candidates flatten recursively when the transform is used in
  candidate-stream mode;
- output framing can emit newline after every emitted candidate, including the
  last emitted candidate;
- callback stop prevents later parsing and later output;
- callback error, source read error, sink write error, parse error, and
  unsupported transform shape are distinguishable;
- malformed JSON after complete prior candidates preserves prior output;
- candidate metadata stays source-relative during nested expansion;
- peak memory is independent of total source size, candidate count, match count,
  and output size.

## Non-Goals

- Do not implement LQL selectors, projections, or mutations in lonejson.
- Do not expose private parser structs as a public workaround.
- Do not require callers to emit raw JSON punctuation, separators, string
  escapes, or object keys outside lonejson writer/control APIs.
- Do not use hidden full-candidate capture, temp files, token replay queues, or
  full-output buffering to make the transform appear streaming.
- Do not require byte-identical preservation of source formatting.
- Do not change existing candidate capture modes.
- Do not change existing chunked number writer behavior.
- Do not require seekable input.

## Definition Of Done

This CR is complete when:

- public headers expose a stable candidate transform API satisfying the semantics
  above;
- documentation states callback ordering, old-value access, output framing,
  nested array expansion, insertion support, stop/error behavior, and memory
  behavior precisely;
- lonejson tests cover all required cases above;
- liblql can remove callback-source projection/mutation all-candidate
  spooled-replay paths without adding full-candidate/full-output
  materialization;
- liblql can preserve current public candidate mutation/projection behavior,
  including trailing newlines, nested root-array flattening, match limits,
  partial-result counters, numeric increment, remove, create, projection, and
  project-then-mutate.
