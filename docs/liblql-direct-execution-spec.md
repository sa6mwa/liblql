# liblql Direct Execution Specification

## Decision

liblql owns all LQL execution: selector compilation, candidate decisions,
projection, mutation, temporal matching, and output policy. Vendored LoneJSON
is restricted to JSON basics: input reading, strict framing, tokenization,
validation, generic value events, spooling, and JSON writing. LoneJSON must
not contain Candidate Run, candidate transforms, selector-shaped options, or
LQL-aware state.

This replaces the candidate-engine and hybrid designs. liblql is unreleased;
there is no compatibility obligation for the removed candidate API.

## Required Execution Model

liblql compiles selector and mutation input into reusable flat programs. One
direct liblql stream state consumes LoneJSON token events for each strict NDJSON
candidate. It owns path stacks, compiled-program state, match truth, mutation
state, and output. No callback forwards a generic LoneJSON transform event into
an LQL decision adapter.

The initial implementation must reproduce Go LQL stream behavior exactly:

- duplicate object keys are evaluated in source order;
- selector truth, temporal semantics, mutation ordering, and deferred errors
  match the accepted Go parity cases;
- matched-only output applies mutations only to selected candidates;
- projection and mutation use the same direct program state;
- file, reader, callback source, CLI, and Lua routes share the same executor.

The direct executor may use bounded per-candidate spooling only for public
payload callbacks or semantics that require delayed output. It must not copy or
retain every candidate by default, cache candidates or results, or materialize
the whole input.

## LoneJSON Retained Surface

Retain only ordinary JSON capabilities required by liblql:

- reader, file, buffer, fd, and callback adapters;
- strict NDJSON framing and root-array rejection support;
- generic value/path visitor parsing and JSON diagnostics;
- writer/escaping support;
- bounded memory and spill-backed spools used by public payload APIs.

Delete from vendored LoneJSON in this cutover:

- `lonejson_candidate_run_*` public APIs, aliases, options, enums, callbacks,
  and implementation;
- candidate transform/action staging, deferred output decisions, transform
  projection, old-value, and candidate writer state;
- candidate-only tests, macros, and documentation.

Candidate stream framing and public payload capture remain only where they are
ordinary LoneJSON JSON-stream features, not LQL transform features.

## Invariants

- Input is strict NDJSON at every liblql entry point. Root arrays are errors;
  there is no flattening.
- RSS is bounded by the direct program, parser/writer stacks, bounded transport
  buffers, and at most one current callback payload spool. It does not grow
  with input size, candidate count, match count, or result count.
- The 100 MiB large-JSON gate remains below 128 MiB. Repeated large candidates
  prove spool reset rather than only a per-candidate bound.
- No selector-specific feature is added to LoneJSON. Selector specialization
  belongs entirely to liblql's compiled program.

## Cutover

1. Implement direct liblql program execution over LoneJSON basic events and
   prove Go behavioral parity before deleting working callers.
2. Route every liblql selector/projection/mutation entry point through that
   executor. Delete `source_candidate_run_*` and all Candidate Run adapters.
3. Delete the vendored Candidate Run/transform surface and its tests.
4. Run vendored C tests, sanitizer/fuzz gates, strict-NDJSON tests, large RSS
   gates, and the complete Go/C benchmark matrix.

## Acceptance

Completion requires:

- no Candidate Run/transform surface remains in vendored LoneJSON or liblql;
- all accepted Go/C parity behavior passes;
- strict NDJSON and bounded RSS gates pass;
- every accepted Go/C benchmark row is at least 1.0x, with 1.2x pursued only
  where the direct program stays simpler and clearer.
