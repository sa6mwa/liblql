# LoneJSON CR: Chunked JSON Number Writer

## Intent

liblql needs a lonejson-owned way to pass through JSON number tokens from a
path-aware parse callback to a writer without retaining the complete number
text in liblql first.

This is a streaming writer feature. It is not a request for lonejson to
understand LQL mutation semantics, and it is not permission for liblql to write
raw JSON bytes around lonejson writer state.

## Current Problem

LoneJSON path visitors expose numbers as `number_begin`, `number_chunk`, and
`number_end` callbacks. LoneJSON writers currently expose
`lonejson_writer_number_text()`, which accepts one complete validated number
token.

That API is correct for ordinary scalar emission, but it forces downstream
transform users to buffer every pass-through number token until `number_end`.
liblql mutation and projection therefore keep inline number buffers and spill to
the receiver allocator for unusually long number tokens, even when the current
operation is a straight pass-through and no numeric mutation is active.

The bounded buffering is safe, but it is not the final C-native streaming shape:
the parser already validated the incoming number token, and lonejson owns the
writer state that knows when a number value may be emitted. liblql should not
work around this by bypassing the writer, by serializing raw token bytes itself,
or by adding a liblql-local JSON writer.

## Required Behavior

Add public writer entry points, or an equivalent public writer mode, with these
semantics:

- `number_begin` opens one JSON number value in the writer at a position where a
  scalar value is legal.
- `number_chunk` appends one fragment of that number token.
- `number_end` closes the number, validates the complete token, and commits it
  to the output if valid.
- If validation fails, the writer reports an error and does not silently emit an
  invalid JSON value.
- Writer structure rules remain lonejson-owned: object/array separators,
  object member ordering, key/value state, top-level value state, and error
  propagation must match existing writer behavior.
- The feature supports fragmented input. Splitting the same valid number across
  arbitrary chunks must produce the same logical output as
  `lonejson_writer_number_text()`.
- Memory remains bounded by current-token validation state and configured
  writer buffers. The API must not require downstream code to retain complete
  source values, whole candidates, or full output values.
- Existing `lonejson_writer_number_text()`, `i64`, `u64`, and `f64` behavior
  remains unchanged.

An implementation may internally retain the current number token if that is how
lonejson validates numbers today. The key requirement is that this buffering is
lonejson-owned, bounded to the current token, and exposed as a streaming writer
contract so downstream transform code does not need to allocate or bypass the
writer.

## Non-Goals

- Do not add LQL selector, projection, or mutation knowledge to lonejson.
- Do not expose raw writer internals.
- Do not require downstream users to pre-validate the complete number token.
- Do not permit downstream users to emit arbitrary raw JSON bytes through the
  writer.
- Do not change number formatting. Pass-through chunks should preserve the
  accepted token text with the same validation semantics as
  `lonejson_writer_number_text()`.
- Do not solve candidate replay or predicate-gated capture. Those are covered
  by separate CRs.

## Validation Required In LoneJSON

The lonejson test suite should prove at least:

- one-chunk and multi-chunk valid numbers produce the same JSON as
  `lonejson_writer_number_text()`;
- fragmented numbers across sign, integer, decimal, fraction, exponent marker,
  exponent sign, and exponent digits are accepted when the whole token is
  valid;
- invalid numbers fail at `number_end` with an actionable writer error;
- writer state is restored or closed consistently after number validation
  failure according to existing writer error rules;
- chunked numbers work as object member values, array elements, and top-level
  values;
- chunked number calls fail when made in invalid writer states, such as before
  an object key where a value is not legal;
- empty number tokens are rejected;
- existing number writer APIs and JSON value writer APIs remain unchanged.

## liblql Acceptance Criteria After LoneJSON Support

Once this surface exists, liblql should replace pass-through number buffering in
projection and mutation writer callbacks with lonejson chunked number emission
where no numeric mutation needs the complete number value.

The liblql acceptance gate should prove:

- projection and mutation parity remain unchanged for integer, decimal,
  exponent, negative, and long number tokens;
- straight pass-through projection/mutation of long number tokens no longer
  allocates receiver memory for number text;
- increment mutation still buffers or parses only the numeric target it must
  modify;
- malformed number behavior remains lonejson-owned and unchanged;
- `make test`, `make asan`, `make bench-check`, and the no-hot-path-allocation
  tests remain green.
