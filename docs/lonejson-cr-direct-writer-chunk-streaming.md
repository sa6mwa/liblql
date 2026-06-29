# LoneJSON CR: Direct Writer Chunk Streaming

## Intent

liblql needs lonejson sink-mode writers to emit pass-through string chunks
directly to the configured sink, with only bounded writer/parser state, while
lonejson continues to own JSON escaping, validation, structure, separators, and
error reporting.

This is a C-native hot-path performance feature. It is not a cache, not output
memoization, not an instruction to materialize candidate output, and not a
request for liblql-specific behavior in lonejson.

## Current Problem

Seekable liblql candidate mutation already avoids candidate capture. For file
ranges, liblql parses candidates with `LONEJSON_CANDIDATE_CAPTURE_NONE`, uses
lonejson's 64-bit candidate offsets and sizes, rereads matched object ranges
with `pread()`, and copies unmatched ranges directly from the source. That path
does not need candidate spooling, full document materialization, or a
selector/result cache.

The remaining release-profile cost for matched-object mutation is in writer
emission. Local liblql profiles with lonejson `v0.35.2` on the seekable
`mutate_file_selector` benchmark show dominant samples in
`lonejson_spooled_append` and `realloc` below `lonejson_writer_string_chunk()`
while liblql is rewriting matched objects through the public lonejson writer.
Representative profile:

- benchmark: `mutate_file_selector '/status="open"'` over the generated large
  NDJSON fixture;
- candidates: `262144`;
- matches: `65536`;
- peak RSS: about `3.5 MiB`;
- dominant owned cost: `lonejson_spooled_append`, allocator growth,
  `memmove`, and sink writes reached from `lonejson_writer_string_chunk()`.

That proves the current liblql hot path is bounded and non-materializing, but
it is not the final performance shape. The cost is not selector evaluation or
mutation dispatch. It is dependency-owned writer chunk handling.

## Required Behavior

Add a public writer mode, writer option, or equivalent writer implementation
guarantee with these semantics:

- In sink mode, `lonejson_writer_string_begin()`,
  `lonejson_writer_string_chunk()`, and `lonejson_writer_string_end()` stream
  accepted string text chunks through the writer to the configured sink without
  appending the whole string or the whole containing value to a
  `lonejson_spooled` buffer.
- Memory remains bounded by writer state, parser/escape state, configured
  chunk buffers, and the current escape fragment. It must not grow with the
  length of the string, containing object, candidate, source stream, or output
  stream.
- JSON escaping and validation remain lonejson responsibilities. Downstream
  consumers must not have to concatenate JSON strings, pre-escape text, or
  bypass the writer.
- Object/array structure, key/value state, commas, top-level completion, sink
  failure handling, writer poisoning, and `lonejson_writer_finish()` semantics
  remain unchanged.
- Fragmented string chunks produce the same logical JSON output as one large
  chunk.
- The common chunk pass-through path should be straight-line and branch-light.
  Designs that require downstream users to add per-token cache lookups,
  selector-result invalidation, or output replay bookkeeping do not satisfy the
  performance intent.
- Existing generator-mode behavior may remain separate if generator
  backpressure requires buffering. The required behavior is specifically for
  sink-mode writers where the sink accepts bytes synchronously.

## Non-Goals

- Do not add LQL selector, projection, or mutation semantics to lonejson.
- Do not add a liblql-specific callback hook.
- Do not require byte-identical output compared with Go or with the source
  input; logical JSON equivalence and existing writer semantics are what
  matter.
- Do not implement this by hiding full-string, full-candidate, or full-output
  buffering behind a streaming-looking API.
- Do not require downstream libraries to implement JSON escaping,
  serialization, tokenization, or candidate framing.
- Do not solve this with runtime caches, candidate/result memoization, or
  retained output objects.

## Validation Required In LoneJSON

The lonejson test suite should prove at least:

- a sink-mode chunked string much larger than the configured in-memory spool
  threshold emits valid JSON without `lonejson_spooled_append()` growth;
- fragmented chunk input across ordinary UTF-8, escape-worthy bytes, and chunk
  boundaries produces the same logical string as one contiguous chunk;
- sink failure during a chunk poisons the writer and prevents a successful
  finish;
- object keys, array values, and nested object string values all preserve
  normal writer comma/key/value behavior;
- peak memory is independent of string length and output size;
- existing `lonejson_writer_string()`, `lonejson_writer_string_reader()`,
  `lonejson_writer_json_value_*()`, and spooled writer APIs preserve their
  documented behavior.

## liblql Acceptance Criteria After LoneJSON Support

Once this behavior exists in lonejson, liblql should keep the current seekable
mutation architecture: parse candidates with no capture, reread matched object
ranges by offset, and rewrite through the public lonejson writer.

The liblql acceptance gate should prove:

- selector, mutation, projection, and CLI parity remain unchanged;
- seekable file mutation still performs no candidate capture and no
  liblql-owned full candidate materialization;
- warmed receiver allocation tests still pass;
- `make bench-check`, `make bench-memory-check`, and `make bench-1g-check`
  remain green;
- release profiles for seekable matched-object mutation are no longer
  dominated by `lonejson_spooled_append` or allocator growth from
  `lonejson_writer_string_chunk()`.
