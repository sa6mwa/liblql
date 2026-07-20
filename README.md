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
flattened. This intentionally diverges from Go lql's JSON-stream behavior:
accepting an array document would weaken the NDJSON framing contract. Input may
contain ordinary JSON whitespace but must be standard JSON, including valid
UTF-8 in strings; emitted records are compact JSON plus one newline.

The implementation must prove Go behavioral parity on accepted parity rows and
at least 1.0x GCC C/Go performance on every accepted benchmark row. JSON scalar
equality intentionally uses liblql's typed JSON semantics instead of the pinned
Go library's selector-string coercion: unquoted numbers, booleans, and null are
typed, quoted values are strings, and numeric equality compares JSON numbers by
numeric value rather than by source spelling. `lql_stream_execute` live heap,
not RSS, is the primary embedded-memory invariant and must remain at or below
256 KiB, including the 100 MiB current-record gate. Every `lql_new` receiver
also owns an independent 8 MiB allocation budget covering parsed selectors,
projections, mutations, execution plans, and compatibility spools. The
explicitly spooled compatibility executor keeps 64 KiB of the current record in
memory before it spills to disk.

## Execution Contracts

`lql_stream_execute` is the public true-streaming API. It validates arbitrary
whitespace-tolerant NDJSON with bounded internal state. Decision callbacks work
with any valid input supported by its bounded one-pass scanner. Plans wider
than that scanner return `LQL_STATUS_UNSUPPORTED` before input is consumed;
use the separately named compatibility API when arbitrary-width execution is
required. Selected-record output and value callbacks require the
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

File-backed mutation values are a liblql feature, not a `clql` preprocessor.
`ctx->mutation_parse(ctx, ...)` accepts pre-split mutation arrays and also
single strings containing comma/newline-separated top-level clauses, with
brace shorthand keeping nested clauses together.
`ctx->mutation_parse(ctx, ...)` rejects `file:`, `textfile:`, and
`base64file:` values by default. Call
`ctx->mutation_parse_with_options(ctx, ..., &options, ...)` with
`options.enable_file_values` set and a base directory for relative file paths
to opt in. The default local backend reopens each path for every evaluation
pass, so it does not retain file descriptors or require rewind support. Set
`options.file_value_open`, `options.file_value_close`, and
`options.file_value_user` to resolve opaque application references into fresh
byte readers instead; this is the supported path for virtual or non-seekable
sources. The callback context remains owned by the caller while the mutation
handle exists.

`options.time_now` injects a C `time_t` clock for `time:...=NOW`; an omitted
clock uses `time(NULL)`. `lql_stream_request.time_now` similarly fixes
selector-relative date terms (`today`, `yesterday`, and `now`) for a stream
execution. LQL timestamps remain RFC3339 UTC and do not depend on the process
locale. A request may also set `cancelled`/`cancel_user` to stop synchronously
between bounded scanner operations; that returns success with
`LQL_STREAM_STOP_CANCELLED`.

## Lifecycle Surface

This repository follows the pkt.systems CMake lifecycle. Linux builds use the
pinned Bootlin GCC toolchains selected by `cmake/cpkt-toolchain.cmake`; host
Clang is only a development tool for `clang-format` and `clangd`.

Useful local gates:

```sh
make lifecycle-check
make lifecycle-version-contract
make test
make lua-cli-smoke
make lua-cli-clql-parity
make lua-cli-parity
make package-source-smoke
make package-verify
make print-release-version
```

`make lifecycle-version-contract` verifies that git worktree builds use only an
exact lightweight `vX.Y.Z` tag on `HEAD`, then `LQL_VERSION_OVERRIDE` for
untagged release-candidate builds, and otherwise `0.0.0`; `/VERSION` is used
only by source archives outside git. `make prerelease` runs the shared release
proof graph without cleaning first. `make release` runs the version contract
first, then cleans generated state and runs the same proof graph.
`make release-upload-list` prints `dist/liblql-<version>-CHECKSUMS` followed by
the artifacts listed in that manifest. Release uploads use that manifest-derived
list, not a `dist/` glob.

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
make lua-cli-clql-parity
make lua-cli-parity
make lua-artifact-smoke
```

The installed Lua CLI is `lql.lua`; it mirrors the supported `clql` selection,
projection, mutation, file-backed mutation, count, and inline workflows, while
keeping the same deliberate exclusions for prettyx/theme behavior. Its output
workflow uses the explicitly spooled liblql API so it can normalize
whitespace-tolerant input; it can therefore spill the current record to a
temporary file. `make lua-cli-clql-parity` compares `lql.lua` output against
`clql`; `make lua-cli-parity` compares `lql.lua` output against the pinned Go
`lql` CLI for shared workflows. Lua itself does not parse or transform JSON in
those comparisons.
