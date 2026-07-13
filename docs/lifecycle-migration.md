# liblql lifecycle migration

This ledger tracks the cutover from the rewrite-era repository lifecycle to the
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
- Dependency archives: current liblql has none.  CMake still resolves the
  shared `CPKT_DEPENDENCY_CACHE` contract once so a future checksum-pinned
  archive can be acquired through the lifecycle cache rather than a
  project-local download path.  Bootlin and AFL++ remain separate
  toolchain-cache concerns.

## Migration map

| Current surface | Target lifecycle surface | Preserved behavior | Verification |
| --- | --- | --- | --- |
| `debug` preset | `debug` preset | Debug build and unit tests | preset contract test, `make test` |
| `release` preset | host/release and target release presets | Optimized benchmark/release build | benchmark gates, package matrix |
| ASan/libFuzzer preset | removed | None; superseded by AFL++ | `make fuzz-smoke`, `make fuzz` |
| host compiler discovery | Bootlin resolver | GCC C89 warning-clean builds | resolver tests, CMake cache inspection |
| rewrite-specific Make targets | standard lifecycle Make surface | Direct reset, no-LoneJSON, parity, profile, live heap | `make test-all`, `make prerelease` |
| no package surface | host binary SDK archive | static/shared liblql SDK | `make package-verify` |
| no target tool helper | `scripts/discover_target_tools.sh` | package verification uses configured target tools | `make target-tool-check` |
| no release matrix | `release-matrix` and `release` | local release proof | checksum/privacy/relocatability gates |
| no CLI | `clql` example/binary | thin `lql_stream_execute` adapter | `make clql-smoke` |
| no Lua surface | Lua facade/source rock | Lua module parity with current expectations | `make lua-test`, Lua artifact verification |
| no fuzz surface | AFL++ fuzz target and corpus | parser/stream robustness | `make fuzz-smoke` |
| no dependency archive cache contract | shared `CPKT_DEPENDENCY_CACHE` resolution | self-contained build does not download archives; future dependencies have one cache boundary | `make dependency-cache-check`, `make dependency-cache-privacy-regression` |

## Open migration tasks

1. Wire Bootlin and AFL++ resolver scripts into CMake toolchain files and Make
   targets.
2. Normalize CMake presets and add preset verification.
3. Replace ASan lifecycle target with AFL++ fuzzing. Done for the first JSON
   stream harness and smoke instrumentation gate; add more harnesses as the
   CLI/Lua/package surfaces return.
4. Restore `clql` and executable examples. Selection/count CLI, Go-compatible
   selection flags, AND/OR selector arguments, and fixture smoke are in place.
   Mutation/projection/theme flags fail explicitly until those stream engine
   paths exist.
5. Restore Lua facade, development rock, Lua tests, and release Lua artifacts.
   Initial Lua 5.5 facade, smoke coverage, source package, rendered rockspec,
   source rock, and repo-local development rock install workflow are in place.
6. Add install rules, CMake package config, pkg-config metadata, and extracted
   SDK consumer tests. Done for install-tree smoke, host binary SDK archive
   verification, and full release matrix package verification.
7. Add target-tool discovery for package generation and verification. Done for
   configured CMake cache values, compiler sibling tools, PATH fallback, and
   target mismatch checks; package verification consumes it for ELF and Mach-O
   loader metadata checks.
8. Add package generation, checksum manifest generation, privacy scans,
   relocatability checks, and release matrix. Initial host package, checksum,
   layout, runtime-path, privacy, and extracted-consumer verification are in
   place; `make release-matrix` now covers all Linux Bootlin targets, Darwin
   when osxcross is available, the source archive, and Lua release artifacts.
9. Make `prerelease` and `release` share the release pipeline, with `release`
   cleaning first. Done with a structural lifecycle regression check.
10. Remove this ledger or convert it into permanent lifecycle documentation once
    all target surfaces pass.
