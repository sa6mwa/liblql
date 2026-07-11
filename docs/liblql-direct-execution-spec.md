# Direct liblql Execution Reset

## Status

This document is the sole execution specification for the reset. The current
tree is intentionally incomplete. No implementation work may begin until the
deletion gate in this document is satisfied and committed. Historical parity,
completion, port, dependency-gap, and benchmark claims were removed because
they described the discarded architecture.

## Product Contract

liblql implements the LQL behavior accepted by the Go reference. It accepts
strict NDJSON only at streaming entry points. A root JSON array is an error;
there is no array flattening in liblql or clql.

The completed implementation must preserve these invariants:

- Go behavioral parity for the accepted selector, projection, mutation, and
  temporal cases;
- bounded RSS independent of total input size, record count, match count, and
  result count;
- no default candidate/result cache, full-input materialization, or hidden
  spool/replay transform path;
- the 100 MiB large-JSON gate remains below 128 MiB RSS, including repeated
  large records so reset behavior is proven;
- every accepted Go/C benchmark row is at least 1.0x in C; 1.2x is the stretch
  target only where the resulting code remains simpler.

## Boundary

Vendored LoneJSON is used through its public `lonejson.h` interface only. It
provides JSON reading, strict NDJSON framing, token/value events, diagnostics,
JSON writing, and its ordinary bounded spooling facilities. liblql owns all
LQL compilation and execution: selector state, temporal behavior, projection,
mutation ordering, output policy, and record lifecycle.

LoneJSON must not contain LQL-aware state, selector-shaped options, mutation
actions, output decisions, or a record transform engine. liblql must not use
LoneJSON implementation headers or private symbols.

## Required Design

The replacement is a direct, compiled LQL program in liblql. A single stream
executor consumes LoneJSON public events and owns the path stack, selector
truth, projection state, mutation state, and output for the current NDJSON
record. File, callback-source, CLI, and Lua entry points call that same
executor.

The executor may use a bounded current-record spool only when an observable
public payload contract or delayed-output semantic requires it. It must not
use capture as the normal decision or mutation mechanism. The design must be
understandable as direct LQL execution, not an adapter around a renamed
transform engine.

## Deletion Gate

Before writing the replacement executor, remove every dependency on the
discarded Candidate Run architecture. The removal includes:

- the Candidate Run public and private surface from vendored LoneJSON;
- liblql Candidate Run adapters and all candidate-mutation receiver methods;
- corresponding capability fields, headers, source wiring, CLI paths, and Lua
  bindings;
- benchmarks, parity adapters, inventories, tests, scripts, fixtures, and
  documentation that exercise or claim the removed path;
- stale release, completion, parity, and performance assertions about that
  path.

After this deletion, the project is expected to have a deliberate functional
hole: no streaming projection/mutation implementation and no public API that
pretends it exists. Selector parsing and other independent public contracts may
remain. The project is not releasable at this point.

The only permitted retained LoneJSON candidate facility is its generic JSON
stream framing/capture API where it is unrelated to LQL transformation. It is
not a liblql execution API and must not be wrapped as one during the reset.

## Static Proof Of Deletion

The deletion commit must include a repository-wide, tracked-file proof that,
outside this specification and git history, there are no Candidate Run tokens,
candidate-transform tokens, old adapter symbols, removed receiver methods, or
references to their old tests and benchmarks. The proof must cover source,
public headers, bindings, CLI, tests, benchmarks, parity tooling, scripts,
README, and documentation.

Build or test failures caused by the removed API are expected during this
phase. Do not preserve a compatibility shim to make them pass. Delete the
dependent tests instead; replacement tests are written only for the new public
behavior after the direct executor exists.

## Implementation And Proof Order

1. Commit the documentation reset and the deletion gate.
2. Commit the complete deletion and record its static absence proof. Stop.
3. In a fresh implementation session, define the small replacement streaming
   public contract and implement one direct executor in large coherent slices.
4. Add observable behavior tests, then run the focused and complete C, Go
   parity, sanitizer, fuzz, RSS, and benchmark gates.
5. Commit only when the direct implementation meets the contract and evidence
   above.

No completion claim is valid before step 5.
