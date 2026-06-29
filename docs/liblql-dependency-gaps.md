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
