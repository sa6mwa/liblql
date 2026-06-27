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
Full selector-library parity is still not claimed until the Lua facade exposes
selector userdata backed by the public C API and the remaining selector oracle
inventory is closed.

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
`clql -m -f` composes mutation and projection in Go-compatible order by
projecting each output candidate first, then mutating the projected value for
matched candidates; this uses a callback-scoped temp-file spill for the
projected candidate rather than retaining it in memory.
`file:`, `textfile:`, and `base64file:` mutation values can be parsed through
the opt-in parse options and `clql -F`, and execute through source-backed
lonejson writers on both seekable file input and the supported spooled stdin
mutation path. `liblql` and `clql` package archives are produced and verified
locally for Linux GNU/musl x86_64, aarch64, armhf, and Darwin arm64 when the
osxcross toolchain is available. Extracted SDK consumer smokes cover direct C,
CMake `find_package`, and pkg-config usage; the source archive is verified
through an extracted-tree build/test smoke. The host `clql` archive carries the
lonejson runtime libraries it needs and is verified with an extracted
`--version` smoke. Binary archives include artifact-local lonejson dependency
provenance under `share/<package>/`.
The Lua tree now includes a Lua 5.5 facade backed by a direct `lql.core` C
module over public liblql APIs;
`lql.new()` returns a C-owned client userdata backed by a public `lql *`
receiver and exposes receiver version/capability queries, selector inspection,
callback decision streams, and callback-scoped seekable payload handles.
Selector userdata construction, JSON round-trips, and full AST traversal remain
part of the selector AST parity work. The parity benchmark surface has Go, C,
and Lua runners over shared generated fixtures.
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
