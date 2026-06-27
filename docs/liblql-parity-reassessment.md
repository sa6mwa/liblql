# liblql Parity Reassessment

This document records the June 2026 reassessment after the temporal selector
gap showed that the previous parity evidence was too coarse.

## Current Finding

The implementation must not be considered complete. The previous parity gates
proved many representative cases, but they did not prove every broad behavior
claim at the same level of rigor. In particular, a row marked `covered` in
`parity/oracle_inventory.tsv` could mean "there is evidence for this family",
not "all observable behavior in this family has an explicit Go/C parity
matrix".

The temporal selector audit found one real divergence:

- Go rejects leap-second timestamps such as `2026-03-11T01:11:60Z`.
- C accepted them.

That divergence is now fixed and protected by `TestSDKTemporalFormatParity`,
C-only selector tests, and parser rejection tests. The important lesson is that
the old proof shape allowed this gap to exist.

## What Parity Must Mean

For this repository, parity is only credible when a behavior family has at
least one of these proof shapes:

1. A Go-vs-C SDK parity matrix that enumerates accepted and rejected forms.
2. A CLI parity matrix that compares `clql` with Go `cmd/lql` for the same
   observable command behavior.
3. A C-only executable specification for public C behavior that is not a Go API
   shape, with the Go-only boundary documented.
4. A benchmark or memory gate only for performance and scalability claims, not
   as a substitute for semantic parity.

Representative examples are useful smoke tests. They are not enough to justify
an exhaustive parity claim.

## High-Risk Areas Requiring Reaudit

These surfaces currently need explicit matrix review before the implementation
can be called complete:

- Selector parse and evaluation:
  shorthand forms, aliases, dotted wrappers, indexed group merges/conflicts,
  quoted values, empty values, wildcard paths, recursive paths, string-term
  path assertions, `contains.any`, `in.any`, logical composition, and temporal
  literals.

- Projection:
  path normalization, duplicate paths, conflict detection, missing fields,
  non-object roots, malformed input, source-backed projection, seekable ranges,
  and output/error invariants.

- Mutation:
  parser forms, quoted paths, root and nested sets, increments, removals,
  wildcard and recursive paths, numeric object-key versus array-index behavior,
  file-backed values, text validation, error precedence, top-level array
  candidate streams, projection-before-mutation, and matches-only behavior.

- Streaming:
  NDJSON and top-level array flattening, nested arrays, mixed scalar/object
  streams, callback-source framing, seekable offset/size contracts, result
  counters, stop precedence, callback failure behavior, malformed JSON
  accounting, and payload replay.

- CLI:
  argument splitting, interspersed flags, stdin/file behavior, inline/write
  mutation, help/version/theme compatibility, multi-file mutation, projection,
  compact output, and error messages.

## Immediate Process Fix

Until each high-risk surface is audited, `parity/oracle_inventory.tsv` must be
read as a work queue, not as a completion certificate. Rows that are supported
by representative evidence but not by a full behavior matrix should be marked
`partial`.

## Completion Criteria

The implementation can be called complete only after:

1. Every `partial` row in `parity/oracle_inventory.tsv` is either upgraded with
   explicit matrix evidence or narrowed to a documented non-applicable Go-only
   boundary.
2. The SDK and CLI coverage manifests use narrow requirement names that describe
   the exact proven behavior.
3. `make parity-test`, `make test`, `make package-source-smoke`, and the release
   gates pass after the audit changes.
