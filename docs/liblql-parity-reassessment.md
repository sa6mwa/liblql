# liblql Parity Reassessment

This document records the June 2026 reassessment after the temporal selector
gap showed that the previous parity evidence was too coarse. The reassessment
is now closed for the v0 public contract: `parity/oracle_inventory.tsv`
contains no `partial` or `gap` rows, and exact Go parser/framing differences
are documented as v0 exclusions in `docs/liblql-dependency-gaps.md`.

## Finding

The original finding was that the implementation could not be considered
complete because the previous parity gates proved many representative cases but
did not prove every broad behavior claim at the same level of rigor. In
particular, a row marked `covered` in `parity/oracle_inventory.tsv` could mean
"there is evidence for this family", not "all observable behavior in this
family has an explicit Go/C parity matrix".

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
import/export, and selector builders, and `lql_selector` is now the canonical
AST consumed by parser, JSON, builders, evaluator, capability inspection, and
public traversal. Lua selector userdata now covers text parse, AST JSON
import/export, traversal, builders, method-style inspection, and query reuse
through that public C API. The final inventory closure is now complete for the
v0 public contract.

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

## Reaudited High-Risk Areas

These surfaces required explicit matrix review before the implementation could
be called complete for the v0 public contract:

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
  mutation, help/version behavior, unsupported theme rejection, multi-file
  mutation, projection, compact output, and error messages.

## Process Fix

During the reassessment, `parity/oracle_inventory.tsv` was treated as a work
queue rather than a completion certificate. Rows supported only by
representative evidence were marked `partial` until they gained explicit
matrix evidence or a documented v0 boundary. That process is now complete:
the current inventory has no `partial` or `gap` rows.

## Progress Since Reassessment

- Selector evaluation now has named Go-vs-C SDK matrices for wildcard and
  recursive path traversal, string-term behavior, and logical composition.
- The C unit contract has matching named selector groups for those behavior
  families, so the C test suite exercises stable public invariants directly.
- The old broad selector SDK tests are now documented as residual regression
  corpora, not as completion evidence.
- The port specification now treats selector AST parity as a first-class SDK
  requirement. Evaluator parity cannot close selector library parity by itself.
- The C receiver API now exposes selector AST builders for match-all,
  logical, string-term, range, date, in, and exists selectors. C-only tests
  cover builder construction and validation, and Go-vs-C SDK parity checks
  prove C builder-created selectors match constructor-equivalent Go selector
  behavior. The JSON contract is structural: byte-identical JSON text is not
  required, but Go-emitted selector JSON must parse into liblql and selector
  JSON emitted by liblql must parse into Go with equivalent selector logic.
- The Lua facade now exposes selector userdata backed by the public C selector
  API. Lua smoke tests cover selector text parsing, selector AST JSON
  round-trips, recursive AST table inspection, all public selector builders,
  selector userdata methods, structured builder/import errors, and reuse of
  built/imported selectors in query workflows.
- The selector JSON and selector constructor oracle rows are now covered:
  Go-emitted selector JSON imports into C, C parsed-selector JSON imports into
  Go, C builder-origin JSON imports into Go, omitted versus explicit-empty
  string-term JSON is covered across contains/icontains/prefix/iprefix, and
  constructor-equivalent C/Lua builders cover every public selector family.
- Go temporal cache tests are closed as observable temporal behavior rather
  than private cache state: C and Go agree on temporal parse/evaluation
  behavior, including builder-created datetime bounds, while Go cache fields
  remain an implementation detail outside the C API.
- Projection, mutation, CLI, streaming, benchmark, and Lua rows are now
  covered for the v0 public contract by their cited C-native tests, CLI/SDK
  parity matrices, Lua tests, benchmark gates, and documented Go-only
  boundaries.
- Exact Go parser/framing behavior is not fully claimed. The accepted v0
  non-parity cases are mixed array-items-then-values candidate framing, Go
  `encoding/json` permissiveness for unmatched surrogates, and Go repeated
  value decoding of leading-zero numeric text. Those are documented in
  `docs/liblql-dependency-gaps.md` and must not be emulated with hidden
  materialization, pre-normalization, or a second parser.

## Completed Criteria

The v0 public-contract reassessment is complete because:

1. Every row in `parity/oracle_inventory.tsv` is either `covered` with cited
   evidence or `not-applicable` for a documented Go-only implementation detail.
2. `lql_selector` is the canonical C selector AST. No private `lql_node`,
   `lql_term`, or `LQL_NODE_*` vocabulary remains as the real AST authority;
   any streaming/query plan is explicitly derived execution state.
3. Public C selector AST traversal, construction, and Go-compatible selector
   JSON parse/serialize APIs remain covered by C-native tests.
4. The Lua facade exposes selector userdata backed by the public C AST API,
   with selector JSON round-trips and AST use in query workflows covered by
   Lua tests.
5. Go-backed SDK parity checks prove selector AST JSON interchange
   structurally: Go-emitted selector JSON imports into liblql and preserves
   behavior, and constructor-equivalent C builder ASTs behave like Go
   selectors.
6. The SDK and CLI coverage manifests use narrow requirement names that describe
   the exact proven behavior.
7. `make parity-test` and `make test` pass after the audit changes. Release
   packaging gates remain separate release-authority work and are tracked in
   `docs/liblql-completion-audit.md`.
