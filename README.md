# liblql

`liblql` is a C89 port of `pkt.systems/lql` built for pkt.systems-style C
SDK releases. It uses `lonejson` for JSON parsing, visiting, serialization, and
stream-aware JSON operations.

Current implementation status: repository lifecycle, the first selector
parse/evaluate slice, a decision-only `FILE *` candidate stream with stop
controls, 64-bit lonejson candidate ranges, callback-scoped seekable range
payload handles for matched candidates, object/array `clql -f` projection, and
`clql -c` compact output for seekable files and non-seekable stdin are in
place.
Object and array-index projection are also exposed through the initial
`lql_projection` C API, and compact range helpers are exposed for seekable and
explicitly buffered JSON values. Mutation parse/plan validation is exposed
through `lql_mutation_plan`, and supported concrete-path mutations including
Go-compatible numeric child handling under arrays and time normalization can be
applied to seekable file ranges without full-document materialization. `clql`
also supports seekable-file inline/write mutation through a temp-file rename.
`file:`, `textfile:`, and `base64file:` mutation values can be parsed through
the opt-in parse options and `clql -F`, and execute through source-backed
lonejson writers. Non-seekable mutation, Lua parity, and full `clql` parity
are still active porting work.

```sh
make deps-debug
make build
make test
```
