# lonejson CR: Streaming Array-Then-Values Candidate Framing

## Intent

liblql needs a public lonejson candidate-stream mode that can parse a
non-seekable source containing a top-level array followed by additional
top-level JSON values, while still emitting each array item and each following
top-level value incrementally as one candidate stream.

The feature is about framing, not capture. It must not require lonejson or a
consumer to materialize the root array, the whole input, or all candidates in
memory before callbacks run.

## Why liblql Needs This

The Go `pkt.systems/lql` stream implementation accepts this input shape:

```text
[{"id":1},{"id":2}]
{"id":3}
{"id":4}
```

as a candidate stream equivalent to:

```text
{"id":1}
{"id":2}
{"id":3}
{"id":4}
```

For liblql, this matters only on non-seekable callback sources. Seekable file
inputs can reconstruct payloads by offset and byte size. Callback sources
cannot rewind, so liblql must rely on lonejson to expose the correct candidate
boundaries as bytes pass through the parser.

lonejson `v0.35.0` exposes useful pieces:

- `AUTO` framing for repeated top-level values;
- `ARRAY_ITEMS` framing for one top-level array treated as item candidates;
- `CAPTURE_NONE`, sink capture, and spooled capture;
- 64-bit candidate `stream_offset`, `byte_size`, and `payload_size`.

What is missing is the composition of `ARRAY_ITEMS` followed by continued
top-level value framing in a single streaming parse.

## Required Behavior

Add a public candidate-framing option with these semantics:

- If the next top-level value is an array, emit each item in that array as a
  candidate as soon as the item's value is complete.
- After the root array closes, continue consuming the same source and emit any
  following top-level JSON values as candidates.
- If a following top-level value is also an array, apply the same array-item
  rule to that array.
- Nested arrays that are themselves candidate values should keep the existing
  candidate-recursion behavior controlled by the current candidate stream API;
  this CR is specifically about continuing after a top-level array closes.
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

## Validation Needed

The lonejson test suite should prove at least:

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

Once lonejson exposes this framing mode, liblql can wire it into
callback-source decision, plus-value, and candidate mutation streams without
changing the public liblql API.
