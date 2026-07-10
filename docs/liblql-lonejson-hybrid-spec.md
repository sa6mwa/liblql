# liblql/lonejson Hybrid Candidate Engine Spec

## Purpose

This document defines the next liblql/lonejson architecture. It supersedes the
old lonejson CR notes for predicate-gated capture, single-pass candidate
transform visitors, chunked number writing, and generic streaming-candidate
processing.

liblql is not released yet. Public C APIs, internal execution paths, and the
vendored lonejson candidate/transform surface may be rewritten when that
reduces complexity, improves performance, or clarifies ownership. Backward
compatibility with the current vendored candidate/transform APIs is not a
requirement.

The goal is a smaller, faster, easier-to-audit hybrid:

```text
input bytes -> lonejson JSON candidate engine -> liblql plan callbacks
           -> lonejson JSON writer/output engine
```

not:

```text
input bytes -> candidate capture -> payload callback -> replay -> second parse
           -> projection/mutation writer
```

except where the public liblql API explicitly exposes a callback-scoped payload
handle.

## Design Decision

The chosen approach is a strict hybrid.

- lonejson owns JSON mechanics.
- liblql owns LQL semantics.
- The boundary between them is a small candidate-plan interface, not an
  expanding set of special-purpose visitor layers.

Rejected alternatives:

- Implement LQL directly in lonejson. This would make lonejson a domain query
  engine, tie LQL semantic changes to lonejson releases, and weaken the JSON
  component boundary.
- Implement all JSON handling in liblql. This would require liblql to own a
  second streaming parser, writer, framer, escaping implementation, and
  validation matrix.

## Ownership Boundary

lonejson owns:

- JSON tokenization, validation, escaping, serialization, and compaction.
- NDJSON candidate framing for repeated top-level values.
- Root-array rejection or other framing-policy enforcement requested by the
  caller.
- Candidate byte accounting: stream offset, candidate byte size, consumed
  bytes, and payload byte size where a payload exists.
- Parser and writer stack state.
- Current-candidate capture when a public payload handle is explicitly needed.
- Streaming transform mechanics: pass-through, suppression, replacement,
  insertion points, output separators, and malformed-output prevention.
- Bounded reader and writer chunk buffering.
- Dependency-owned diagnostics for JSON/framing failures.

liblql owns:

- Selector parsing, ASTs, validation, and planning.
- Path matching, predicate dispatch, scalar comparison, temporal behavior, and
  truth-state finalization.
- Projection path semantics.
- Mutation path semantics, operation ordering, and error precedence.
- Public C receiver API, CLI behavior, Lua facade behavior, and parity policy.
- Deciding which public operations need payload handles versus direct output.
- Performance acceptance against Go.

lonejson must not contain LQL selector, projection, mutation, or parity
knowledge. liblql must not contain a JSON parser, JSON serializer, JSON
escaper, candidate framer, or hidden full-document normalization layer.

## Candidate Model

The canonical liblql stream shape is NDJSON: repeated top-level JSON values.

Root arrays are not valid NDJSON candidate streams for liblql or clql. They
must be hard errors in candidate-stream entry points and must not appear in
Go/C parity tests or performance benchmarks. liblql must not emulate root-array
flattening by spooling, materializing, or reparsing the array.

Nested arrays inside an object candidate remain ordinary JSON values. They are
not candidate streams unless a future public API explicitly defines such a
mode.

Candidate metadata is source-relative and stable:

- `index`: logical candidate number in stream order.
- `stream_offset`: byte offset of the candidate JSON value.
- `byte_size`: byte length of the candidate JSON value, excluding surrounding
  whitespace and NDJSON separators.
- `payload_size`: byte length of an exposed payload, when a payload exists.
- `bytes_read`: total source bytes consumed, including whitespace and trailing
  accepted delimiters where the active framing mode accepts them.

## Required Engine Shape

There should be one dominant candidate execution path in liblql:

```text
parse candidate once
observe values through a compiled liblql plan
finalize candidate decision
emit, suppress, project, or mutate through lonejson writer mechanics
advance to next candidate
```

The same engine must support these operation families:

- decision-only queries;
- plus-value queries over seekable inputs through byte ranges;
- plus-value queries over non-seekable inputs through explicit payload capture
  only when the public API requires a payload callback;
- projection;
- mutation;
- projection-before-mutation;
- matches-only output;
- stop limits and callback failure propagation.

The engine must avoid a second parse for normal projection and mutation. If a
shape cannot be transformed correctly in one pass, the operation must either:

- return an explicit unsupported status for that shape; or
- route through a clearly named public spooled/captured API path whose cost is
  documented and benchmarked separately.

It must not silently hide full-candidate buffering, output spooling, or
temporary-file staging behind an API that claims to be streaming.

## lonejson Candidate Surface

The vendored lonejson candidate/transform code is effectively project-local for
this phase. It may be rewritten substantially. The desired public-upstreamable
surface is smaller than the current accumulated candidate API.

Keep or design toward:

- one candidate runner for reader/file/fd/path inputs where practical;
- explicit framing mode, with liblql using strict NDJSON;
- one observer interface for path/value events needed by liblql plans;
- one transform interface for output policy and value replacement;
- one payload-capture mode used only for public payload-handle APIs;
- one writer/sink abstraction for output bytes;
- explicit stop/error propagation;
- clear bounded-memory guarantees;
- candidate metadata as a first-class result.

Remove or collapse:

- legacy root-array flattening support for liblql candidate streams;
- duplicate candidate replay paths used only because old transform surfaces
  were insufficient;
- capture modes that exist only as intermediate implementation scaffolding;
- transform options that encode historical liblql workarounds rather than
  generic JSON mechanics;
- nested delegated candidate parsers used to simulate framing behavior;
- callbacks that expose mutable parser internals instead of stable event or
  policy data;
- compatibility shims for candidate/transform APIs that no other consumer uses.

The upstream lonejson handoff should be one coherent surface, not a collection
of CR fragments. Upstream lonejson can then implement the same semantics under
its own review, sanitizer, fuzzing, and portability matrix.

## liblql Simplification Targets

The current liblql implementation has too many overlapping execution shapes.
The rewrite should reduce these to a small set:

- `candidate_decide`: parse and count/query decisions without payload capture.
- `candidate_emit`: parse and emit matched payloads by seekable range or
  explicit capture.
- `candidate_transform`: parse, observe, and write projected/mutated output in
  one pass.
- `buffered_value`: intentionally buffered single JSON helper APIs only.

Everything else should justify its existence by exposing a distinct public
contract. Internal paths that differ only because one input is `FILE *` and
another is a callback source should share the same candidate engine after the
read abstraction boundary.

Candidate spooling is a fallback or public payload mechanism, not the default
internal transform strategy.

## Performance Contract

Performance is a product requirement. C must be faster than Go on the parity
benchmark matrix; below `1.0x` is failure, and `1.5x` or better is the working
threshold for a healthy margin.

Optimization policy:

- Prefer deleting branches, callbacks, replays, parse passes, and temporary
  state over adding caches.
- Do not add selector-result caches, candidate-result caches, or output caches
  to compensate for an over-general pipeline.
- Receiver-owned scratch reuse is allowed when it removes allocation or
  repeated derivation without hot-path invalidation complexity.
- Memory must remain bounded by parser/writer stack, current-candidate state,
  configured chunk buffers, and liblql plan scratch.
- RSS gates remain product gates. A speedup that breaks bounded-memory
  invariants is not acceptable.

Every performance change should answer:

1. What work did this remove?
2. Which benchmark rows improved?
3. Which rows regressed?
4. Did code/state-machine complexity go down, stay flat, or go up?
5. Which tests prove semantics did not change?

Changes that add complexity must clear a higher bar: they should replace a
larger old path or be rejected.

## Verification Plan

The rewrite is complete only when these gates pass:

- public C SDK tests for query, projection, mutation, payloads, stop controls,
  malformed JSON, and root-array rejection;
- Go-backed parity tests for the accepted public contract;
- clql smoke tests, including root-array hard errors for stdin candidate flows;
- allocator/RSS gates proving bounded memory;
- Go/C/Lua benchmark counter validation;
- Go/C performance comparison proving no required C row is below `1.0x`;
- sanitizer gates for project-owned code;
- package/source verification where release-facing metadata changes.

During the rewrite, benchmark fixtures and parity rows must exclude root-array
flattening. If Go still supports flattening internally, that remains a Go-only
behavior outside the liblql parity matrix.

## Migration Sequence

1. Freeze this spec as the authority for candidate-engine work.
2. Remove stale docs and test expectations that preserve root-array flattening
   or old candidate CR surfaces.
3. Audit current liblql execution paths and classify each as keep, collapse,
   or delete.
4. Rewrite vendored lonejson candidate/transform internals toward the smaller
   surface above.
5. Collapse liblql source/file/projection/mutation paths onto the single
   candidate engine.
6. Re-run full tests, sanitizer gates, memory gates, and benchmark parity.
7. Document the final lonejson surface as an upstream handoff.
8. After upstream lonejson ships the surface, switch normal presets back to the
   upstream package and prove the performance sticks.

## Non-Goals

- No LQL semantics in lonejson.
- No bespoke JSON parser or writer in liblql.
- No hidden full-source, full-candidate, or full-output materialization.
- No cache-heavy performance strategy.
- No root-array flattening in liblql/clql NDJSON candidate streams.
- No compatibility promise for the current vendored lonejson candidate API.
