# LoneJSON CR: Predicate-Gated Candidate Capture

## Intent

liblql needs a lonejson-owned candidate capture mode where selector
observation can run while a non-seekable candidate is parsed, and the caller can
decide at candidate end whether the validated candidate payload should be
retained for callback-scoped replay.

This is a streaming performance feature. It is not a cache, not a request for
lonejson to understand LQL selectors, and not permission for liblql to retain
whole candidates, spool the whole source, add a second JSON parser, or stage
output in temporary files.

## Current Problem

Seekable inputs do not need candidate capture for plus-value payloads or
matches-only mutation: lonejson reports 64-bit candidate offsets and byte
sizes, and liblql can reread matched ranges with `pread()` without disturbing
the parser.

Callback-source inputs cannot seek or rewind. When the public liblql API must
expose a callback-scoped payload handle, project a matched payload, or mutate a
matched candidate from a non-seekable source, liblql currently has to request
`LONEJSON_CANDIDATE_CAPTURE_SPOOLED` before selector truth is known. Sparse
selectors therefore spool unmatched candidates and discard them after selector
evaluation.

Local liblql profiles with lonejson `v0.35.2` on callback-source plus-value and
matches-only mutation rows show dominant samples in `lonejson_spooled_append`,
`realloc`, and memory movement from all-candidate source capture. liblql-owned
selector evaluation and callback dispatch are not the dominant cost.

## Required Behavior

Add a public candidate-stream capture mode, callback, or equivalent public API
with these semantics:

- The parser still validates and frames each candidate exactly once.
- Path-aware visitor callbacks run while candidate bytes are consumed, so
  downstream code can evaluate selector state without replay.
- At candidate end, after path visitor state is final and before a payload
  handle is exposed, lonejson asks the caller whether to retain or discard the
  current candidate payload.
- If the caller says retain, the candidate end callback receives the same kind
  of callback-scoped replayable payload handle exposed by existing spooled
  capture.
- If the caller says discard, no payload handle is exposed, and lonejson
  releases any dependency-owned replay state for that candidate before scanning
  the next candidate.
- Candidate metadata keeps the existing meanings: index, stream offset,
  candidate byte size, payload size when applicable, callback ordering, stop
  behavior, and error behavior.
- The mode works for reader, buffer, file, path, and fd candidate streams,
  including fragmented reader input.
- Memory remains bounded by parser state, configured current-candidate replay
  policy, and chunk buffers. It must not grow with total source size, total
  candidate count, or total match count.
- Existing capture modes keep their current behavior. This is an additional
  public surface, not a semantic change to `CAPTURE_NONE`, sink capture, memory
  capture, or spooled capture.

## Non-Goals

- Do not add LQL selector, projection, or mutation semantics to lonejson.
- Do not expose private parser internals as the public API.
- Do not require downstream libraries to manage partially captured JSON bytes.
- Do not implement this with full-source buffering, hidden temp files, retained
  candidate arrays, or selector-result caches.
- Do not require byte-identical JSON text compared with Go or the source input.
  Retained payloads need to preserve the same logical replay contract as
  existing spooled capture.

## Validation Required In LoneJSON

The lonejson test suite should prove at least:

- sparse-match fixture discards unmatched candidates without exposing payload
  handles;
- dense-match fixture retains all candidates and replays them identically to
  existing spooled capture;
- fragmented reader fixture preserves retain/discard behavior across arbitrary
  chunk boundaries;
- stop from the retain/discard callback prevents later candidates from being
  parsed;
- callback failure reports through the existing candidate-stream error surface;
- retained payloads obey configured spool memory and max-byte policies;
- discarded candidates do not leak temporary files or live allocation;
- candidate offsets and sizes are identical to existing capture modes;
- `CAPTURE_NONE` and existing spooled capture performance remain unchanged.

## liblql Acceptance Criteria After LoneJSON Support

Once this behavior exists in lonejson, liblql should replace callback-source
plus-value, sparse projection, and matches-only mutation paths that currently
require all-candidate spooled capture with selector-gated retain/discard.

The liblql acceptance gate should prove:

- selector, payload, projection, mutation, and CLI parity remain unchanged;
- callback-source plus-value and sparse matches-only mutation no longer spool
  discarded candidates;
- no liblql-owned full candidate materialization, hidden output buffering,
  temp-file staging, or selector-result cache is introduced;
- warmed receiver allocation tests still pass;
- `make bench-check`, `make bench-memory-check`, and the 1 GiB memory gate
  remain green;
- release profiles for sparse callback-source plus-value and matches-only
  mutation rows no longer show all-candidate `lonejson_spooled_append` as the
  dominant cost.
