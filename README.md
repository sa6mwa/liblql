# liblql

`liblql` is a C implementation of LQL. The active v0 rewrite executes compiled
LQL directly over a self-contained C89 JSON scanner/emitter in liblql. Direct
execution does not include `lonejson.h`, link `liblonejson`, or use LoneJSON at
runtime.

## Reset State

The previous LoneJSON-backed execution architecture has been removed. This
branch is rebuilding direct execution around one scanner state that performs
strict NDJSON framing, JSON validation, selector observation, and compact
emission without a parser/writer event boundary in the hot path. The governing
contract and completion gates are in
[`docs/liblql-self-contained-execution-spec.md`](docs/liblql-self-contained-execution-spec.md).

Streaming inputs are strict NDJSON. Root arrays are hard errors and are never
flattened. Input may contain ordinary JSON whitespace; emitted records are
compact JSON plus one newline.

The implementation must prove Go behavioral parity on accepted parity rows and
at least 1.0x GCC C/Go performance on every accepted benchmark row. JSON scalar
equality intentionally uses liblql's typed JSON semantics instead of the pinned
Go library's selector-string coercion: unquoted numbers, booleans, and null are
typed, quoted values are strings, and numeric equality compares JSON numbers by
numeric value rather than by source spelling. Live heap, not RSS, is the
primary embedded-memory invariant and must remain at or below 256 KiB,
including the 100 MiB current-record gate.

## Execution Contracts

`lql_stream_execute` is the public true-streaming API. It validates arbitrary
whitespace-tolerant NDJSON with bounded internal state. Decision callbacks work
with any valid input. Selected-record output and value callbacks require the
caller to provide compact input and a `range_writer` that can replay the
validated source range. Missing compact-source configuration returns
`LQL_STATUS_UNSUPPORTED` before consuming input; a false compact-input
assertion fails after validating that record and emits none of it. Projection
and mutation output also return `LQL_STATUS_UNSUPPORTED` until they have
incremental emitters.

`lql_stream_execute_spooled` is a separately named compatibility API. It may
materialize one normalized record and spill it to a temporary file for selected
output, callbacks, projection, or mutation. Use it only when that local disk
side effect is acceptable. `clql` and the Lua selected-output facade use this
compatibility API to retain their whitespace-normalizing behavior.

`lql_new()` returns a receiver shell. Prefer `ctx->stream_execute(ctx, ...)`
and `ctx->stream_execute_spooled(ctx, ...)`, alongside the other receiver
methods; the identically signed `lql_stream_execute` free functions remain
compatibility entry points.

## Lifecycle Surface

This repository follows the pkt.systems CMake lifecycle. Linux builds use the
pinned Bootlin GCC toolchains selected by `cmake/cpkt-toolchain.cmake`; host
Clang is only a development tool for `clang-format` and `clangd`.

Useful local gates:

```sh
make lifecycle-check
make test
make lua-cli-smoke
make package-source-smoke
make package-verify
make print-release-version
```

`make prerelease` runs the shared release proof graph without cleaning first.
`make release` cleans generated state and then runs the same proof graph.
Release uploads are selected from `dist/liblql-<version>-CHECKSUMS`, not from a
`dist/` glob.

Each target produces two C artifacts. `liblql-<version>-<target>.tar.gz` is a
library-only SDK containing headers, `liblql.a`, shared-library ABI files,
CMake/pkg-config metadata, and documentation. `clql-<version>-<target>.tar.gz`
contains only `bin/clql` and `share/doc/clql/`; Linux CLI binaries are fully
static (including glibc and musl targets), while Darwin keeps its normal system
loader linkage. The CLI archive does not include liblql SDK files.

## Lua Facade

The Lua 5.5 facade is a LuaRocks source module over the public shared
`liblql` ABI. The C module builds against installed public headers and links
to `liblql.so`; it does not compile private liblql sources or statically embed
`liblql.a`.

```sh
make lua-rock
make lua-test
make lua-cli-smoke
make lua-artifact-smoke
```

The installed Lua CLI is `lql.lua`; it mirrors the supported `clql` selector
workflow. Its selected-output workflow uses the explicitly spooled liblql API
so it can normalize whitespace-tolerant input; it can therefore spill the
current record to a temporary file.
