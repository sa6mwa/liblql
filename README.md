# liblql

`liblql` is a C implementation of LQL. The active v0 rewrite executes compiled
LQL directly over a self-contained C89 JSON scanner/emitter in liblql. Direct
execution does not include `lonejson.h`, link `liblonejson`, or use LoneJSON at
runtime.

## Reset State

The previous LoneJSON-backed execution architecture has been removed. This
branch is rebuilding direct execution around one scanner state that performs
strict NDJSON framing, JSON validation, selector observation, compact payload
emission, projection, and mutation without a parser/writer event boundary in
the hot path. The governing contract and completion gates are in
[`docs/liblql-self-contained-execution-spec.md`](docs/liblql-self-contained-execution-spec.md).

Streaming inputs are strict NDJSON. Root arrays are hard errors and are never
flattened. Input may contain ordinary JSON whitespace; emitted records are
compact JSON plus one newline.

The implementation must prove Go behavioral parity and at least 1.0x GCC C/Go
performance on every accepted benchmark row. Live heap, not RSS, is the primary
embedded-memory invariant and must remain at or below 256 KiB, including the
100 MiB current-record gate.

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
workflow and streams file/stdin input through liblql.
