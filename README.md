# liblql

`liblql` is a C89 port of `pkt.systems/lql` built for pkt.systems-style C
SDK releases. It uses `lonejson` for JSON parsing, visiting, serialization, and
stream-aware JSON operations.

Current implementation status: repository lifecycle, the first selector
parse/evaluate slice, a decision-only `FILE *` candidate stream with stop
controls, callback-source decision and spooled-payload match streams, 64-bit
lonejson candidate ranges, callback-scoped seekable range payload handles for
matched file candidates, object/array `clql -f` projection, and
`clql -c` compact output for seekable files and non-seekable stdin are in
place.
Object and array-index projection are also exposed through the initial
`lql_projection` C API for seekable and explicitly buffered JSON values, and
compact range helpers are exposed for seekable and explicitly buffered JSON
values. The SDK installs generated version metadata in
`lql/version.h` and exposes `lql_version()` plus
`lql_capabilities_get()`. Mutation parse/plan validation is exposed through
`lql_mutation_plan`, and supported concrete-path mutations
including Go-compatible numeric child handling under arrays and time
normalization can be applied to seekable file ranges without full-document
materialization, or to explicitly caller-buffered JSON values through
`lql_mutate_json()`. `clql` also supports seekable-file inline/write mutation
through a temp-file rename.
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
verified with an extracted `--version` smoke. The Lua tree now includes an
initial `clql`-backed facade with deterministic smoke tests, and the parity
benchmark surface has Go, C, and Lua runners over shared generated fixtures.
The direct Lua C module, Lua release artifacts, full cross-target release
matrix, and full `clql` parity are still active porting work.

```sh
make deps-debug
make build
make test
```

`make test` is the fast C/API contract. Go-backed parity remains available
through `make parity-test` and is included in `make test-all`.
