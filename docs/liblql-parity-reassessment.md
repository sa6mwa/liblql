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

The selector AST audit found a larger public-surface gap:

- Go exposes a public recursive `Selector` AST with public `Term`,
  `RangeTerm`, `DateTerm`, and `InTerm` values.
- Go selector parsing returns that AST, Go users can construct and inspect it,
  and Go can marshal/unmarshal selector AST JSON.
- liblql previously had internal AST-like selector nodes, but the installed C
  API exposed only opaque selector handles, evaluator/capability behavior, and
  no public AST traversal, construction, or Go-compatible selector JSON
  parse/serialize surface.

That is not a Go-only implementation detail. It is public library behavior that
needs an idiomatic C representation and a Lua userdata facade backed by that C
surface. The C receiver API now covers traversal, selector AST JSON
import/export, and selector builders; Lua selector userdata and final inventory
closure remain open.

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

- Selector AST, parse, and evaluation:
  public AST traversal/construction, selector AST JSON, shorthand forms,
  aliases, dotted wrappers, indexed group merges/conflicts, quoted values,
  empty values, wildcard paths, recursive paths, string-term path assertions,
  omitted-value versus explicit-empty-value semantics, `contains.any`,
  `in.any`, logical composition, and temporal literals.

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

## Progress Since Reassessment

- Selector evaluation now has named Go-vs-C SDK matrices for wildcard and
  recursive path traversal, string-term behavior, and logical composition.
- The C unit contract has matching named selector groups for those behavior
  families, so the C test suite exercises stable public invariants directly.
- The old broad selector SDK tests are now documented as residual regression
  corpora, not as completion evidence.
- The port specification now treats selector AST parity as a first-class SDK
  requirement. Evaluator parity cannot close selector library parity while the
  Lua userdata facade is missing.
- The C receiver API now exposes selector AST builders for match-all,
  logical, string-term, range, date, in, and exists selectors. C-only tests
  cover builder construction and validation, and Go-vs-C SDK parity checks
  prove C builder-created selectors match constructor-equivalent Go selector
  behavior. The JSON contract is structural: byte-identical JSON text is not
  required, but Go-emitted selector JSON must parse into liblql and selector
  JSON emitted by liblql must parse into Go with equivalent selector logic.

## Completion Criteria

The implementation can be called complete only after:

1. Every `partial` row in `parity/oracle_inventory.tsv` is either upgraded with
   explicit matrix evidence or narrowed to a documented non-applicable Go-only
   boundary.
2. Public C selector AST traversal, construction, and Go-compatible selector
   JSON parse/serialize APIs remain covered by C-native tests.
3. The Lua facade exposes selector userdata backed by the public C AST API,
   with selector JSON round-trips and AST use in query workflows covered by
   Lua tests.
4. Go-backed SDK parity checks prove selector AST JSON interchange
   structurally: Go-emitted selector JSON imports into liblql and preserves
   behavior, and constructor-equivalent C builder ASTs behave like Go
   selectors.
5. The SDK and CLI coverage manifests use narrow requirement names that describe
   the exact proven behavior.
6. `make parity-test`, `make test`, `make package-source-smoke`, and the release
   gates pass after the audit changes.
