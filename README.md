# liblql

`liblql` is a C89 port of `pkt.systems/lql` built for pkt.systems-style C
SDK releases. It uses `lonejson` for JSON parsing, visiting, serialization, and
stream-aware JSON operations.

Current implementation status: repository lifecycle, the first selector
parse/evaluate slice, a decision-only `FILE *` candidate stream with stop
controls, 64-bit lonejson candidate ranges, and object/array `clql -f`
projection plus `clql -c` compact output for seekable files are in place.
Object and array-index projection are also exposed through the initial
`lql_projection` C API, and compact range helpers are exposed for seekable and
explicitly buffered JSON values. Mutation parse/plan validation is exposed
through `lql_mutation_plan`, and supported concrete-path mutations including
time normalization can be applied to seekable file ranges without full-document
materialization. Wildcard mutation execution, file-backed mutation values,
payload handles, and full `clql` parity are still active porting work.

```sh
make deps-debug
make build
make test
```
