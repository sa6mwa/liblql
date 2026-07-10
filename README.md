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
spooled payloads, caller-managed payload sinks, stop controls, NDJSON/repeated
top-level JSON candidate streams, and large-input memory gates without hidden
full-document materialization. Root arrays are rejected as candidate-stream
input because they are not NDJSON.

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
Literal-only selector paths also skip evaluator container-type stack tracking
and disable object/array end callbacks, because wildcard and recursive path
matching are the selector forms that need parent container types.
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
The same scalar path-prepare pass also records the active observer families,
scratch sizes, and bounded contains, prefix, exact, temporal, numeric-range, and
exists slices for that scalar. Scalar begin, chunk/end, boolean/null, and
container observers iterate only their family slice instead of rediscovering
path matches, scanning the full selector predicate set, or branching away
unrelated selector families. These slices are current-path scratch, not cache
lookup or result memoization.
`contains.any` selectors also carry fixed first-byte lookup masks so scalar
scans reject impossible bytes without walking every alternative.
Small case-sensitive `contains.any` selectors use the same literal scanner as
the equivalent explicit OR form, preserving branch-light `memchr`-driven scans
instead of adding per-byte alternative dispatch or result caches.
Candidate hit, stream-miss, and `in` alternative scratch also use candidate
epoch marks, so candidate reset is O(1) in steady state instead of clearing
selector-sized buffers for every candidate.
Receiver-owned evaluator scratch carries those epochs across warmed queries, so
query setup does not clear selector-sized hit and stream-state arrays unless
scratch grows or an epoch wraps.
Scalar value observers compute each current scalar's text length once when
contains, prefix, or exact families need it, rather than rescanning the same
value separately for each observer family. For selectors whose truth is sticky
once true, candidate finalization reuses the already-observed positive match
state instead of walking the selector tree again at candidate end.
Selectors also carry container-observer feature bits, so scalar-only selectors
use root-only object/array callbacks instead of running container path
preparation on every object and array boundary.
Seekable mutation setup initializes only live mutation state and plan scratch,
not the full inline frame reserve. Path frames are assigned when pushed, so this
removes per-candidate zeroing without adding retained candidate state.
Seekable file candidate mutation skips unmatched non-array candidates before
compact rewrite fallback when `matches_only` is enabled. This keeps zero-match
mutation on the no-capture scan path instead of routing misses through spooled
writer replay.
Mutation writer passes hold the caller-provided `FILE *` lock once for the
duration of the rewrite, reducing repeated stdio lock work while preserving the
existing FILE API and streaming directly to the caller's output. Linux builds
use unlocked stdio writes inside that already-locked region, including
seekable candidate range copies and separators, avoiding per-write lock
branches without adding output caches or materialized buffers.
Internal spooled rewrite passes use the same output-lock shape for spooled
payload copies and candidate separators, keeping public payload APIs unchanged
while removing repeated stdio lock traffic from source/file rewrite paths.
Public `FILE *` payload copies also hold the caller-provided output lock once
while copying spooled or seekable payload bytes, preserving the public payload
API while avoiding repeated output-side lock branches.
Callback-source decision streams inspect one bounded prefix chunk, reject root
arrays as invalid NDJSON candidate streams, and use no-capture parsing for
ordinary streams.
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
test and parity targets, while `make build-debug-lua` builds the Lua-enabled
debug preset used by Lua smoke and Go/C/Lua benchmark gates. `make install`
installs that `clql` to
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
Performance work favors reducing hot-path branches, repeated selector walks,
copies, and allocator traffic. liblql does not use selector/result caches,
candidate/result memoization, or hidden full-document materialization to make
streaming paths appear faster. Receiver-owned scratch reuse is allowed only
when it removes allocation or repeated derivation without changing the
observable selector result and without adding invalidation logic to the hot
path.
Dependency-owned streaming work is tracked as explicit lonejson CRs rather than
hidden liblql workarounds. With lonejson `v0.41.0`, callback-source matched
payload queries use predicate-gated spooled capture, so sparse unmatched
candidates are discarded before payload handles are exposed or replayed.
Callback-source source mutation and projected mutation use lonejson's candidate
transform path with finalized gated candidate policy, projected-candidate
composition, recursive candidate framing, and lonejson-owned replay where late
selector decisions require it. Projection and mutation pass-through numbers use
lonejson's chunked number writer instead of liblql-owned complete-token
buffers. liblql configures lonejson's public 64 KiB candidate reader buffer so
large candidate streams avoid tiny transport reads without adding caches or
full-value materialization.
In all cases liblql must keep streaming behavior real and bounded, not add
caches or materialized output buffers.
Selected-scalar query predicates use bounded streaming state in liblql rather
than full selected-value buffers, including numeric `range`; the current
lonejson visitor still imposes a small raw number-token limit documented in
`docs/liblql-dependency-gaps.md`.
The fast C test gate also proves the warmed decision hot path does not attempt
receiver allocation for file, callback-source, compound, and mixed
scalar-observer selectors. Warmed projection and seekable candidate mutation
paths are covered by the same no-receiver-allocation contract.

Common verification targets:

```sh
make parity-test
make test-all
make bench-check
make bench-memory-check
make bench-large-json-check
make bench-freeze-baseline
make release-matrix
make release
```

`make release` is the clean local release gate. In an untagged git worktree the
resolved version is intentionally `0.0.0`; publishable artifacts require an
exact lightweight `vX.Y.Z` tag on `HEAD` and the formal release flow.
