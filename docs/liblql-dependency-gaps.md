# liblql v0 Parser And Framing Non-Parity

This document records the deliberately excluded Go parity edges for the v0 C
port. These are not selector, projection, mutation, or ordinary streaming
semantic gaps. They are parser/framing compatibility edges inherited from the
Go implementation's use of `encoding/json.Decoder` and from input shapes that
mix framing modes.

The v0 public contract is:

- liblql uses lonejson strict JSON parsing and framing;
- liblql supports repeated top-level JSON values as the public candidate-stream
  shape;
- liblql rejects root arrays in NDJSON candidate-stream entry points rather
  than flattening them into candidate items;
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

lonejson `v0.41.0` exposes useful pieces:

- `AUTO` framing for repeated top-level values;
- `ARRAY_ITEMS` framing for one top-level array treated as item candidates;
- `RECURSIVE_ARRAY_ITEMS` framing for one top-level array whose nested array
  items are recursively flattened into logical candidates;
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
not currently matched by lonejson `v0.41.0`:

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

liblql now configures lonejson's public `json_value_max_number_bytes` runtime
limit to 4096 bytes for receiver-owned parser runtimes. The C allocator gate
proves 2048-digit numeric range matching succeeds without peak memory growing
with the numeric token. This closes the previous 200-byte practical ceiling for
the v0 contract.

The remaining limitation is only for truly arbitrary-size numeric tokens:
raising the configured limit scales lonejson-owned parser workspace, so liblql
does not claim unbounded numeric-token range matching. A future unbounded
contract still needs a lonejson visitor mode that streams raw number-token
chunks with a caller-configurable 64-bit byte limit and no allocation
proportional to that limit. liblql must not bypass lonejson with a second JSON
tokenizer to emulate that behavior.

## Candidate Engine Rewrite Direction

The old standalone lonejson CR notes for predicate-gated capture, single-pass
candidate transforms, chunked number writing, and streaming-candidate
processing have been retired. They were useful while liblql depended on
upstream-only lonejson releases, but the current vendored preset lets liblql
reshape the candidate/transform surface directly.

The governing document is now
[`docs/liblql-lonejson-hybrid-spec.md`](liblql-lonejson-hybrid-spec.md).

The new dependency boundary is:

- lonejson owns JSON parsing, validation, NDJSON candidate framing, writing,
  escaping, byte accounting, bounded current-candidate capture, and generic
  candidate transform mechanics;
- liblql owns selectors, projection semantics, mutation semantics, public API
  policy, CLI behavior, and benchmark acceptance;
- the vendored lonejson candidate/transform API has no compatibility promise
  during this rewrite, because it is only used by liblql;
- candidate spooling is a public payload-handle mechanism or explicit fallback,
  not the default internal transform path;
- root-array flattening is not part of liblql/clql NDJSON candidate streams.

Further work in this area should simplify and collapse execution paths rather
than add more CR-shaped layers. The intended upstream lonejson handoff is a
single coherent candidate engine surface after the vendored implementation has
proved semantics, RSS behavior, and Go/C performance.

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
work is therefore C-owned performance work: reduce branches, syscalls, stdio
lock traffic, and repeated state checks while preserving the same no-capture,
no-cache, no-materialization architecture.
