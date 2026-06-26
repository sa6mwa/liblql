# liblql

`liblql` is a C89 port of `pkt.systems/lql` built for pkt.systems-style C
SDK releases. It uses `lonejson` for JSON parsing, visiting, serialization, and
stream-aware JSON operations.

Current implementation status: repository lifecycle, the first selector
parse/evaluate slice, a decision-only `FILE *` candidate stream with stop
controls, callback-source decision and spooled-payload match streams, 64-bit
lonejson candidate ranges, callback-scoped seekable range payload handles for
matched file candidates, caller-managed payload sink callbacks, object/array
`clql -f` projection, and
`clql -c` compact output for seekable files and non-seekable stdin are in
place.
Object and array-index projection are also exposed through the initial
`lql_projection` C API for seekable ranges, caller-provided read callbacks, and
explicitly buffered JSON values, and compact range helpers are exposed for
seekable ranges, caller-provided read callbacks, and explicitly buffered JSON
values. The SDK installs generated version metadata in
`lql/version.h` and exposes `lql_version()` plus
`lql_capabilities_get()`. Mutation parse/plan validation is exposed through
`lql_mutation_plan`, and supported concrete-path mutations
including Go-compatible numeric child handling under arrays and time
normalization can be applied to seekable file ranges without full-document
materialization, to seekable candidate streams through
`ctx->mutate_file_range_candidates()`, to caller-provided read callbacks through
`ctx->mutate_source_paths()`, to callback-source candidate streams through
`ctx->mutate_source_candidates()`, or to explicitly caller-buffered JSON values
through `ctx->mutate_json()`. `clql` also supports seekable-file inline/write
mutation through a temp-file rename.
The public C API now exposes an instantiatable receiver shell through
`lql_new()`, with examples and bindings using `ctx->method(ctx, ...)`.
Selector/query/projection/mutation operations are not exported as free-function
wrappers, and allocator wrapper functions are not part of the public API.
Project-owned allocation is centralized through the internal liblql allocator
surface, and `make test` rejects direct runtime allocator calls outside the
allocator module.
The same supported streaming mutation subset can run over non-seekable stdin
through callback-scoped spooled candidate payloads.
`clql -m -f` composes mutation and projection in Go-compatible order by
projecting each output candidate first, then mutating the projected value for
matched candidates; this uses a callback-scoped temp-file spill for the
projected candidate rather than retaining it in memory.
`file:`, `textfile:`, and `base64file:` mutation values can be parsed through
the opt-in parse options and `clql -F`, and execute through source-backed
lonejson writers on both seekable file input and the supported spooled stdin
mutation path. Host `liblql` and `clql` package archives are produced and
verified locally, including extracted SDK consumer smokes; the source archive
is produced and verified through an extracted-tree build/test smoke. The full
host `clql` archive carries the lonejson runtime libraries it needs and is
verified with an extracted `--version` smoke. Binary archives include
artifact-local lonejson dependency provenance under `share/<package>/`.
The Lua tree now includes a Lua 5.5 facade backed by a direct `lql.core` C
module over public liblql APIs;
`lql.new()` returns a C-owned client userdata backed by a public `lql *`
receiver and covers callback decision streams plus callback-scoped seekable
payload handles. The parity benchmark surface has Go, C, and Lua runners over
shared generated fixtures.
The standalone Lua source package, rendered release rockspec, and LuaRocks
source rock are produced and verified locally. The release matrix builds and
verifies Linux GNU/musl artifacts for x86_64, aarch64, and armhf with
target-correct compilers; Darwin remains conditional on a working osxcross
toolchain. Full `clql` parity is still active porting work.

```sh
make deps-debug
make build
make test
```

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
