# liblql lifecycle migration

This ledger tracks the cutover from the scanner-era repository lifecycle to the
pkt.systems CMake lifecycle. It is intentionally temporary unless retained as
release documentation after the migration is complete.

## Scope decisions

- Release targets: all OS/architecture targets named by the lifecycle:
  `x86_64-linux-gnu`, `x86_64-linux-musl`, `aarch64-linux-gnu`,
  `aarch64-linux-musl`, `armhf-linux-gnu`, `armhf-linux-musl`, and
  `arm64-apple-darwin` when the local osxcross toolchain is available.
- Compiler policy: Linux builds use cached Bootlin GCC collections only.
  Host Clang is allowed only for `clang-format` and native-host `clangd`
  development checks.
- Library artifacts: ship both static and shared liblql SDK artifacts.
- Consumer metadata: ship both CMake package config and pkg-config metadata.
- CLI/examples: restore examples and `clql`, targeting near parity with the Go
  LQL CLI while keeping `clql` as a thin adapter over `lql_stream_execute`.
- Lua: restore the Lua facade and Lua release artifacts according to the
  lifecycle Lua contract.
- Fuzzing: remove ASan/libFuzzer-style lifecycle gates in favor of AFL++ GCC
  plugin fuzzing through the cached Bootlin x86_64 GNU toolchain.
- Existing invariants: preserve self-contained direct execution, no LoneJSON
  runtime/link dependency, strict whitespace-tolerant NDJSON semantics, GCC
  C-vs-Go accepted-row parity at or above 1.0x, and live heap at or below
  256 KiB.

## Migration map

| Current surface | Target lifecycle surface | Preserved behavior | Verification |
| --- | --- | --- | --- |
| `debug-scanner` preset | `debug` preset | Debug build and unit tests | preset contract test, `make test` |
| `release-scanner` preset | host/release and target release presets | Optimized benchmark/release build | benchmark gates, package matrix |
| `asan-scanner` preset | removed | None; superseded by AFL++ | `make fuzz-smoke`, `make fuzz` |
| host compiler discovery | Bootlin resolver | GCC C89 warning-clean builds | resolver tests, CMake cache inspection |
| scanner-specific Make targets | standard lifecycle Make surface | Direct reset, no-LoneJSON, parity, profile, live heap | `make test-all`, `make prerelease` |
| no package surface | host binary SDK archive | static/shared liblql SDK | `make package-verify` |
| no target tool helper | `scripts/discover_target_tools.sh` | package verification uses configured target tools | `make target-tool-check` |
| no release matrix | `release-matrix` and `release` | local release proof | checksum/privacy/relocatability gates |
| no CLI | `clql` example/binary | thin `lql_stream_execute` adapter | `make clql-smoke` |
| no Lua surface | Lua facade/source rock | Lua module parity with current expectations | `make lua-test`, Lua artifact verification |
| no fuzz surface | AFL++ fuzz target and corpus | parser/stream robustness | `make fuzz-smoke` |

## Open migration tasks

1. Wire Bootlin and AFL++ resolver scripts into CMake toolchain files and Make
   targets.
2. Normalize CMake presets and add preset verification.
3. Replace ASan lifecycle target with AFL++ fuzzing. Done for the first JSON
   stream harness and smoke instrumentation gate; add more harnesses as the
   CLI/Lua/package surfaces return.
4. Restore `clql` and executable examples. First thin selected-output/count
   CLI and fixture smoke are in place; Go CLI near-parity still needs option
   expansion.
5. Restore Lua facade, development rock, Lua tests, and release Lua artifacts.
   Initial Lua 5.5 facade and smoke coverage are in place; LuaRocks
   development/release artifacts remain.
6. Add install rules, CMake package config, pkg-config metadata, and extracted
   SDK consumer tests. Done for install-tree smoke and initial host binary SDK
   archive verification; full release matrix remains.
7. Add target-tool discovery for package generation and verification. Done for
   configured CMake cache values, compiler sibling tools, PATH fallback, and
   target mismatch checks; package verification still needs to consume it.
8. Add package generation, checksum manifest generation, privacy scans,
   relocatability checks, and release matrix. Initial host package, checksum,
   layout, runtime-path, privacy, and extracted-consumer verification are in
   place; `make release-matrix` now covers all Linux Bootlin targets and
   Darwin when osxcross is available. Source/Lua release artifacts and final
   `make release` remain.
9. Make `prerelease` and `release` share the release pipeline, with `release`
   cleaning first.
10. Remove this ledger or convert it into permanent lifecycle documentation once
    all target surfaces pass.
