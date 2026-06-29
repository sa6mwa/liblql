# liblql

`liblql` is a C89 port of `pkt.systems/lql` built for pkt.systems-style C
SDK releases. It uses `lonejson` for JSON parsing, visiting, serialization, and
stream-aware JSON operations.

Current implementation status: the public C SDK, `clql`, Go oracle parity,
benchmark gates, packaging, and release rehearsal surfaces are in place for the
current public contract. Selectors cover equality, contains, icontains, prefix,
iprefix, range, date, in, exists, AND/OR/NOT composition, JSON Pointer paths,
array indexes, wildcards, recursive descent, shorthand operators, and date
macros. Query streams support seekable `FILE *` inputs, callback-source
decision streams, callback-scoped seekable range payloads, callback-scoped
spooled payloads, caller-managed payload sinks, stop controls, root-array
candidate streams, nested top-level array flattening, and large-input memory
gates without hidden full-document materialization.

## Selector AST Architecture

`lql_selector` is the required canonical selector AST. Text parsing, selector
JSON parsing, and public AST builders must all produce `lql_selector`; selector
evaluation, JSON serialization, traversal, capability inspection, and Lua
userdata must consume that same AST. Streaming and performance code may derive
a compiled plan from a selector, but that plan is execution state, not a second
selector representation.

```text
text selector parser  \
selector JSON parser  -> lql_selector AST -> evaluation
public AST builders   /                   -> JSON serialization
                                            -> public traversal/cursors
                                            -> Lua selector userdata
                                            -> optional lql_selector_plan
```

The C implementation uses `lql_selector` as the recursive selector AST
internally; the previous private `lql_node`/`lql_term` tree has been removed.
The Lua facade exposes selector userdata backed by the public C API for text
parse, AST JSON import/export, AST traversal, builders, and query reuse.
Selector-library parity for the v0 public contract is covered by C-only and
Go-backed SDK matrices.

Projection, compaction, and mutation are exposed through the public receiver
API for seekable ranges, caller-provided read callbacks, callback-source
candidate streams where applicable, and explicitly buffered JSON helper APIs
where the API name is intentionally buffered. Mutation plans support concrete
paths, numeric increment/add, remove, time normalization, brace shorthand,
array/object wildcard behavior, recursive mutation behavior where claimed, and
opt-in `file:`, `textfile:`, and `base64file:` values. `clql` supports
selector, projection, compaction, inline/write mutation for seekable files, and
the supported stdin streaming mutation paths.
The public C API now exposes an instantiatable receiver shell through
`lql_new()`, with examples and bindings using `ctx->method(ctx, ...)`.
Selector/query/projection/mutation operations are not exported as free-function
wrappers. Selector capability and execution-trait inspection is exposed through
receiver methods that inspect the parsed selector handle without allocation.
Allocator wrapper functions are not part of the public API. Project-owned
allocation flows through the active receiver's allocator, and `make test`
rejects direct runtime allocator calls outside the allocator module.
Query execution is chunk-driven for selected string and number predicates:
`contains`, `prefix`, `eq`, `!=`, `in`, temporal comparison, and numeric range
matching use bounded selector-sized stream scratch rather than materializing the
selected scalar. The hot path has no liblql-owned full-scalar buffer fallback.
During evaluation, liblql derives a borrowed flat predicate view from the
`lql_selector` AST so scalar callbacks do not repeatedly recurse through the
selector tree; this is execution scratch, not a second selector model.
Selectors also precompute field-path segment offsets and segment kinds into
their normalized field strings, so common absolute paths and simple wildcard
segments match lonejson decoded path segments without reparsing JSON-pointer
text in the hot path. Recursive or escaped paths still use the general matcher.
Selectors finalize the flattened predicate pointer list once, so evaluation
setup does not walk compound selector trees for each candidate.
Selector finalization also records literal lengths, max `any` literal length,
and max `in` fanout so scalar observers use selector-owned facts instead of
recomputing them per candidate.
`any` alternatives also record raw and case-folded first bytes so
`contains.any` scanning does not rebuild needle metadata per scalar chunk.
It also records predicate feature bits, allowing scalar evaluation to skip
entire contains, prefix, exact, temporal, numeric-range, and exists observer
families when a selector cannot use them.
For string and number values, the evaluator derives each predicate's path-match
result once at scalar begin and reuses that hit-index bitmap across chunk and
end observers.
Scalar path-match scratch uses epoch marks, so repeated scalar callbacks do not
clear the full selector hit set in the hot path.
The same scalar path-prepare pass also records the active observer families and
scratch sizes for that scalar, so scalar begin does not perform separate
contains, prefix, exact, temporal, and numeric-range predicate discovery walks.
It also records a compact current-scalar predicate list. Scalar chunk/end
observers and boolean/null scalar observers walk that list instead of
rediscovering path matches or scanning the full selector predicate set.
`contains.any` selectors also carry fixed first-byte lookup masks so scalar
scans reject impossible bytes without walking every alternative.
Candidate hit, stream-miss, and `in` alternative scratch also use candidate
epoch marks, so candidate reset is O(1) in steady state instead of clearing
selector-sized buffers for every candidate.
Receiver-owned evaluator scratch carries those epochs across warmed queries, so
query setup does not clear selector-sized hit and stream-state arrays unless
scratch grows or an epoch wraps.
Callback-source decision streams inspect one bounded prefix chunk and use
no-capture parsing for ordinary non-array streams, while preserving sink capture
for root-array recursion on non-seekable inputs.
The C allocator contract tests warm representative query paths, freeze the
receiver allocator, and then rerun candidate scans; any attempted liblql-owned
allocation in the warmed decision hot path fails `make test`.
`clql -m -f` composes mutation and projection in Go-compatible order by
projecting each output candidate first, then mutating the projected value for
matched candidates; this uses a callback-scoped temp-file spill for the
projected candidate rather than retaining it in memory.
`file:`, `textfile:`, and `base64file:` mutation values can be parsed through
the opt-in parse options and `clql -F`, and execute through source-backed
lonejson writers on both seekable file input and the supported spooled stdin
mutation path. `liblql` and `clql` package archives are produced and verified
locally for Linux GNU/musl x86_64, aarch64, armhf, and Darwin arm64 when the
osxcross toolchain is available. The `clql` archive ships a single executable:
`liblql` and lonejson are linked into `clql`, while Darwin still uses the normal
system dynamic libraries. Darwin SDK packages are verify-only after install:
`liblql` is linked with an `@rpath` install name, and `package-verify` inspects
extracted Mach-O install names, dependency paths, and rpaths with target-correct
`otool`. Extracted SDK consumer smokes cover direct C, CMake `find_package`,
and pkg-config usage; the source archive is verified through an extracted-tree
build/test smoke. Binary archives include artifact-local lonejson dependency
provenance under `share/<package>/`.
The Lua tree now includes a Lua 5.5 facade backed by a direct `lql.core` C
module over public liblql APIs;
`lql.new()` returns a C-owned client userdata backed by a public `lql *`
receiver and exposes receiver version/capability queries, selector inspection,
callback decision streams, and callback-scoped seekable payload handles.
Selector userdata construction, JSON round-trips, and AST traversal are covered
by Lua smoke tests over the public C receiver surface. The parity benchmark
surface has Go, C, and Lua runners over shared generated fixtures.
The standalone Lua source package, rendered release rockspec, and LuaRocks
source rock are produced and verified locally. The release matrix builds and
verifies configured target artifacts with target-correct compilers and
inspection tools. Release upload asset selection is derived from the verified
checksum manifest with `scripts/package.sh print-release-assets`; it is not
selected from a `dist/` glob.

```sh
make deps-debug
make build
make test
```

`make build` produces an installable static `clql` at
`build/clql-static/clql`. On Linux it selects the host architecture and tries a
musl toolchain first, falling back to the host GNU compiler when static linking
is available. `make build-debug` keeps the normal debug SDK build used by the
test and parity targets. `make install` installs that `clql` to
`/usr/local/bin` by default; use `PREFIX`, `BINDIR`, or `DESTDIR` to stage or
change the destination.

`make test` is the fast C/API contract test surface. These tests are the
authority for public C behavior: ownership, callbacks, streaming,
bounded-memory semantics, error handling, and observable results. Go-backed
parity remains available through `make parity-test` and is included in
`make test-all`; it is an oracle for semantic convergence with
`pkt.systems/lql`, not a substitute for C API contract tests and not a source
for mechanically duplicated C unit rows.

Benchmarks use Go as the behavioral reference, not the C performance target.
Mature public liblql paths are expected to be C-native: bounded-memory by
design and substantially faster than Go except where measurements are dominated
by documented external costs such as process startup or disk I/O. Behavioral
benchmark parity and C performance acceptance are separate gates.
Selected-scalar query predicates use bounded streaming state in liblql rather
than full selected-value buffers, including numeric `range`; the current
lonejson visitor still imposes a small raw number-token limit documented in
`docs/liblql-dependency-gaps.md`.
The fast C test gate also proves the warmed decision hot path does not attempt
receiver allocation for file, callback-source, root-array source, compound, and
mixed scalar-observer selectors.

Common verification targets:

```sh
make parity-test
make test-all
make bench-check
make bench-memory-check
make bench-1g-check
make release-matrix
make release
```

`make release` is the clean local release gate. In an untagged git worktree the
resolved version is intentionally `0.0.0`; publishable artifacts require an
exact lightweight `vX.Y.Z` tag on `HEAD` and the formal release flow.
