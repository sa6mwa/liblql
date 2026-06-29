# liblql v0 Parser And Framing Non-Parity

This document records the deliberately excluded Go parity edges for the v0 C
port. These are not selector, projection, mutation, or ordinary streaming
semantic gaps. They are parser/framing compatibility edges inherited from the
Go implementation's use of `encoding/json.Decoder` and from input shapes that
mix framing modes.

The v0 public contract is:

- liblql uses lonejson strict JSON parsing and framing;
- liblql supports repeated top-level JSON values and root-array item streams
  as separate candidate-stream shapes;
- liblql does not support a single candidate stream that starts with root-array
  items and then continues with additional top-level JSON values;
- liblql must not emulate these excluded cases by materializing the whole
  source, adding a second JSON parser, or pre-normalizing input.

The excluded cases can be revisited later if lonejson grows explicit streaming
framing or compatibility modes and liblql deliberately expands its public
contract.

## Mixed Array-Then-Values Candidate Framing

The current liblql v0 candidate-stream contract intentionally does not claim
the Go implementation's mixed framing case where a stream contains a top-level
array followed by additional top-level JSON values:

```text
[
  {"id":1},
  {"id":2}
]
{"id":3}
```

That input shape would be equivalent to a candidate stream containing three
objects. It matters for all current lonejson candidate stream entry points:
`AUTO` framing treats the first top-level array as an array-item candidate
stream and then rejects additional top-level values after the array. Seekable
file inputs can reconstruct emitted candidate payloads from offset and byte
size, but lonejson does not currently expose the enclosing root-array close
offset needed to resume parsing the following top-level values without adding a
second parser in liblql. Callback sources also cannot rewind after a root array
closes.

The missing capability is dependency-owned framing, not capture. For v0, this
shape is intentionally outside the public candidate-stream contract. liblql must
not emulate it by materializing the root array, spooling the whole input, or
retaining all candidates.

lonejson `v0.35.2` exposes useful pieces:

- `AUTO` framing for repeated top-level values;
- `ARRAY_ITEMS` framing for one top-level array treated as item candidates;
- `CAPTURE_NONE`, sink capture, and spooled capture;
- 64-bit candidate `stream_offset`, `byte_size`, and `payload_size`.

What is missing is the composition of `ARRAY_ITEMS` followed by continued
top-level value framing in a single streaming parse.

## Required Dependency Behavior

A future dependency capability would need these semantics:

- If the next top-level value is an array, emit each item in that array as a
  candidate as soon as the item's value is complete.
- After the root array closes, continue consuming the same source and emit any
  following top-level JSON values as candidates.
- If a following top-level value is also an array, apply the same array-item
  rule to that array.
- Nested arrays that are themselves candidate values should keep the existing
  candidate-recursion behavior controlled by the current candidate stream API;
  this future framing capability is specifically about continuing after a
  top-level array closes.
- Preserve existing candidate metadata contracts: candidate index, stream
  offset, byte size, payload size, and callback ordering must describe the
  emitted candidate value, not the enclosing array.
- Preserve existing capture modes. The new framing must work with
  `CAPTURE_NONE`, sink capture, and spooled capture.
- Preserve bounded memory. Parser stack state, chunk buffers, and current-value
  capture/spooling are acceptable; retaining the whole root array, whole input,
  or all candidates is not.
- Surface malformed JSON and trailing invalid data through the existing
  candidate-stream error mechanism after all previously complete candidates
  have been emitted.

## Non-Goals

- Do not add a liblql-specific parser hook.
- Do not add full-document materialization.
- Do not require temp files as a hidden substitute for streaming.
- Do not change existing `AUTO`, `NDJSON`, `SINGLE_VALUE`, or `ARRAY_ITEMS`
  semantics.
- Do not treat this excluded v0 non-parity case as remaining liblql
  implementation work unless the public liblql candidate-stream contract is
  deliberately expanded.

## Validation Needed

If the dependency grows this capability, its test suite should prove at least:

- one array followed by one object emits each array item and the object;
- multiple top-level arrays followed by scalars/objects emit all values in
  order;
- fragmented input across array close and next-value start works;
- callback stop/error propagation still stops promptly;
- `CAPTURE_NONE` does not retain candidate payload bytes;
- sink and spooled capture deliver only the current candidate bytes;
- candidate offsets and sizes exclude enclosing array delimiters and separators;
- malformed JSON after complete candidates reports an error while preserving
  already-emitted callbacks.

Once the dependency exposes this framing mode, liblql can decide whether to
expand seekable decision, plus-value, callback-source, and candidate mutation
streams to claim this additional Go-compatible input shape.

## Go Stdlib JSON Compatibility Edges

The Go `pkt.systems/lql v0.17.1` stream parity corpus compares `QueryStream`
against `encoding/json.Decoder`. Two observable edge cases from that corpus are
not currently matched by lonejson `v0.35.2`:

- The string payload
  `{"id":"a","s":"line\n\t\u0001\u2028\u2029\ud800\udc00\ud800x"}`
  is accepted by Go's decoder, while lonejson rejects the trailing unmatched
  high-surrogate sequence as an invalid Unicode surrogate pair.
- The stream text `01` is accepted by Go's repeated-value decoder behavior as
  two adjacent numeric values, while lonejson rejects it as an invalid JSON
  number.

These are dependency compatibility decisions because liblql intentionally uses
lonejson as the JSON parser. For v0, liblql intentionally follows lonejson's
strict behavior rather than Go decoder permissiveness for these malformed or
ambiguous inputs. liblql should not add an alternate JSON parser or
pre-normalization layer to mimic these edge cases. If exact Go stdlib
compatibility is required later, lonejson needs an explicit compatibility mode
with documented semantics for these cases.

## Large Numeric Token Streaming

liblql numeric `range` evaluation is implemented as a bounded streaming
consumer of lonejson number chunks: it keeps a small leading slice for ordinary
`strtod()` parity, plus a fixed significant-digit window and decimal/exponent
state for oversized tokens. liblql must not materialize the selected number
text.

The remaining limitation is in lonejson `v0.35.2`: the public path-value visitor
currently rejects raw JSON number tokens above its internal maximum byte limit,
and raising the public config field beyond that limit is not safe on the
current release. A 200-byte accepted token also shows bounded lonejson-owned
allocation before liblql receives the completed numeric scalar. Therefore
arbitrarily large numeric-token range matching is not proven end to end yet.

The required lonejson follow-up is a public arbitrary-value/path-value visitor
mode that streams raw number-token chunks with a caller-configurable 64-bit byte
limit and no allocation proportional to the number token. That feature belongs
in lonejson because lonejson owns JSON tokenization, validation, and visitor
delivery; liblql should not bypass lonejson with a second JSON tokenizer.

## Predicate-Gated Candidate Capture

The standalone CR for this dependency feature is
[`docs/lonejson-cr-predicate-gated-candidate-capture.md`](lonejson-cr-predicate-gated-candidate-capture.md).

Seekable liblql candidate streams can avoid candidate capture: lonejson reports
64-bit candidate offsets and byte sizes, and liblql can reread a matched range
with `pread()` without disturbing the active parser cursor. Callback-source
candidate streams do not have that option. When the public API must expose a
matched payload handle, project a matched payload, or mutate matched candidates
from a non-seekable source, liblql currently has to request
`LONEJSON_CANDIDATE_CAPTURE_SPOOLED` for every candidate before selector truth
is known.

That is semantically correct and bounded in memory, but it is still unnecessary
work for sparse selectors: unmatched candidates are spooled and then discarded.
liblql must not "fix" this by retaining whole candidates itself, delaying output
behind an undisclosed full buffer, using temporary files as a hidden substitute
for streaming, or adding a second JSON parser. The missing capability is a
lonejson-owned capture decision point integrated with candidate parsing.

The required lonejson follow-up is a candidate-stream capture mode where a
caller can decide, at candidate end, whether the current candidate's already
validated bytes should be retained for callback-scoped replay. The important
intent is not "make spooling conditional" as an implementation detail; the
intent is to let a streaming visitor evaluate selector state while lonejson
keeps only the minimal dependency-owned replay state needed to make an
end-of-candidate retain/discard decision.

Required semantics:

- The parser still streams path/value visitor callbacks as candidate bytes are
  consumed. liblql evaluates selectors from those callbacks.
- At candidate end, lonejson invokes a caller decision callback after all
  visitor state for that candidate is final but before any callback-scoped
  payload handle is exposed or discarded.
- If the callback says retain, the candidate end callback receives the same
  kind of `lonejson_spooled` payload handle exposed by
  `LONEJSON_CANDIDATE_CAPTURE_SPOOLED`.
- If the callback says discard, no payload handle is exposed and lonejson
  releases any dependency-owned replay state for that candidate before scanning
  the next candidate.
- The API must preserve candidate index, `stream_offset`, `byte_size`, and
  `payload_size` semantics exactly.
- The API must work for reader, buffer, file, path, and fd candidate streams,
  including fragmented reader input.
- Stop and error propagation from the decision callback must follow existing
  candidate callback rules.
- Memory must remain bounded by parser state, configured current-candidate
  replay policy, and chunk buffers; retaining all candidates, the whole source,
  or all matched results is not allowed.
- The feature must be public and supported through the normal lonejson runtime
  allocator and spool policy configuration.

Non-goals:

- Do not ask lonejson to understand liblql selectors.
- Do not expose private parser internals or require liblql to replay token
  events.
- Do not make liblql manage partially captured JSON bytes.
- Do not change existing `CAPTURE_NONE`, `CAPTURE_SINK`,
  `CAPTURE_MEMORY`, or `CAPTURE_SPOOLED` behavior.
- Do not require byte-identical pretty/compact output; the retained payload
  must remain a valid replayable candidate with the same logical value, matching
  the existing candidate capture contract.

Validation needed in lonejson:

- sparse-match fixture proves discarded candidates do not produce payload
  handles;
- dense-match fixture proves retained candidates replay identically to existing
  spooled capture;
- fragmented reader fixture proves the retain/discard decision works across
  arbitrary chunk boundaries;
- stop from the decision callback prevents scanning later candidates;
- decision callback failure reports through the candidate-stream error surface;
- retained payloads obey configured spool memory and max-byte policies;
- discarded candidates do not leak temporary files or live allocation;
- offsets and sizes are identical to existing candidate capture modes;
- `CAPTURE_NONE` performance remains unchanged.

Once lonejson exposes this surface, liblql can replace callback-source
plus-value, projection, and matches-only mutation paths that currently use
all-candidate spooled capture with selector-gated retain/discard. That is the
remaining dependency-owned performance path for sparse non-seekable candidate
streams.

## Single-Pass Candidate Transform Visitors

The standalone CR for this dependency feature is
[`docs/lonejson-cr-single-pass-candidate-transform.md`](lonejson-cr-single-pass-candidate-transform.md).

The predicate-gated capture feature above removes wasted replay work for sparse
selectors. It does not solve dense or all-match transforms where every
candidate is retained. Current callback-source projection and candidate
mutation have to parse each candidate once for selector evaluation while
lonejson spools the candidate bytes, then parse the spooled candidate again to
project or mutate it. Local `perf` profiles on `lonejson v0.35.2` show the
dominant cost in these paths is `lonejson_spooled_append`, allocator growth
under that append path, and replay/write work, not liblql selector or mutation
dispatch.

That two-pass shape is semantically correct and bounded, but it is not the
final C-native performance shape for dense non-seekable source transforms.
liblql must not "fix" it by retaining whole candidates itself, building a
parallel JSON parser, materializing projection state as full JSON values, or
using temporary files as a hidden staging layer. It also must not add a
selector-result, candidate, or transform-output cache to avoid replay: those
would add memory growth, invalidation, and branch cost while failing to remove
the fundamental extra parse/write pass. The missing capability is a
lonejson-owned way for one parse of a candidate to feed multiple path-aware
consumers and a writer.

The required lonejson follow-up is a candidate-stream transform mode that lets a
consumer attach more than one path-aware visitor to the same candidate parse, or
otherwise compose visitor pipelines so selector evaluation and projection or
mutation output can happen from the same validated token stream.

Required semantics:

- The parser validates and frames each candidate once.
- The same candidate parse can drive selector-style observation and one
  transform writer without replaying candidate bytes through a second parser.
- The transform side can suppress, replace, or pass through the current value
  according to callback decisions while preserving normal JSON writer
  validation and escaping rules.
- The public surface must enable straight-line common-case dispatch for
  pass-through and selected-value replacement. A design that forces downstream
  consumers to perform per-token cache lookups, selector-result invalidation
  checks, or replay bookkeeping in the candidate hot path does not satisfy the
  performance intent.
- The API preserves existing candidate metadata: index, stream offset, byte
  size, payload size when applicable, stop/error propagation, and fragmented
  reader behavior.
- Memory remains bounded by parser stack, writer state, selector/projection/
  mutation scratch, and configured current-value buffers. Retaining the whole
  source, all candidates, or all matched outputs is not allowed.
- Existing candidate capture modes keep their current behavior. This feature is
  an additional single-pass transform surface, not a semantic change to
  `CAPTURE_SPOOLED`.

Non-goals:

- Do not add liblql-specific selector or mutation knowledge to lonejson.
- Do not require byte-identical Go JSON output.
- Do not expose raw parser internals that make downstream libraries implement
  their own JSON tokenizer or serializer.
- Do not make this feature depend on seekable input; the main need is
  callback-source streams that cannot rewind.

Validation needed in lonejson:

- dense-match projection fixture proves one parse can select and write
  projected output without candidate spooling or replay;
- dense-match mutation fixture proves one parse can mutate matched object
  candidates and pass through or suppress others according to caller policy;
- sparse-match fixture composes cleanly with retain/discard behavior if both
  features are present;
- fragmented reader fixtures prove transform output is independent of input
  chunk boundaries;
- stop and callback failure propagate without emitting later candidates;
- malformed JSON after prior complete candidates preserves prior callbacks and
  reports the parse error;
- peak memory is independent of total source size and does not grow with the
  number of candidates.

Once lonejson exposes this surface, liblql can replace the current
callback-source projection and mutation replay paths with true single-pass
source transforms. That is the remaining dependency-owned performance path for
dense non-seekable candidate transforms.

## Seekable Matched Mutation

Seekable file mutation is a separate performance path from non-seekable source
transforms. liblql already uses lonejson candidate offsets with
`LONEJSON_CANDIDATE_CAPTURE_NONE` for the root-object seekable mutation path:
unmatched candidates are copied from the original file range, and matched
objects are reread by offset and rewritten through the public lonejson writer.
There is no candidate/result cache, no full-document materialization, and no
candidate capture in that scanning path.

Earlier local profiling conflated this path with an unmatched compact fallback
that could route through writer replay. After the C-side `matches_only`
miss-skip and output-lock fixes, seekable matched-object mutation profiles no
longer identify `lonejson_spooled_append()` as the dominant cost. The current
work is therefore not a lonejson direct-writer CR. It remains C-owned
performance work: reduce branches, syscalls, stdio lock traffic, and repeated
state checks while preserving the same no-capture, no-cache, no-materialization
architecture.
