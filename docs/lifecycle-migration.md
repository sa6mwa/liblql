# liblql lifecycle migration

This ledger records the completed cutover from the rewrite-era repository
lifecycle to the pkt.systems CMake lifecycle. It is retained as release
documentation for the decisions and verification surfaces that now define the
repository.

## Scope decisions

- Release targets: all OS/architecture targets named by the lifecycle:
  `x86_64-linux-gnu`, `x86_64-linux-musl`, `aarch64-linux-gnu`,
  `aarch64-linux-musl`, `armhf-linux-gnu`, `armhf-linux-musl`, and
  `arm64-apple-darwin` when the local osxcross toolchain is available.
- Compiler policy: Linux builds use cached Bootlin GCC collections only.
  Shared Bootlin and AFL++ cache publication is serialized with bounded
  per-collection `flock` locks (`CPKT_TOOLCHAIN_LOCK_TIMEOUT`, default
  600 seconds), validates cache hits before use, and publishes only completed
  roots. AFL++ roots include the selected Bootlin collection identity, so a
  compiler collection update cannot reuse an incompatible plugin build. Host
  Clang is allowed only for `clang-format` and native-host `clangd`
  development checks.
- Library artifacts: ship both static and shared liblql SDK artifacts.
- Consumer metadata: ship both CMake package config and pkg-config metadata.
- CLI/examples: restore examples and `clql`, targeting near parity with the Go
  LQL CLI while keeping `clql` as a thin adapter over the public liblql stream
  APIs. The current CLI and Lua workflows invoke the explicitly named
  `lql_stream_apply_spooled` API so selected output can normalize arbitrary
  whitespace; decision-only library callers should use
  `lql_stream_apply` directly.
- Lua: restore the Lua facade and Lua release artifacts according to the
  lifecycle Lua contract.
- Fuzzing: remove ASan/libFuzzer-style lifecycle gates in favor of AFL++ GCC
  plugin fuzzing through the cached Bootlin x86_64 GNU toolchain.
- Existing invariants: preserve self-contained direct execution, no LoneJSON
  runtime/link dependency, strict whitespace-tolerant NDJSON semantics, GCC
  C-vs-Go accepted-row parity at or above 1.0x, documented liblql JSON scalar
  equality behavior outside the pinned-Go accepted rows, and live heap at or
  below 256 KiB.
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
| no CLI | `clql` example/binary | thin public stream adapter; selected output is explicitly spooled | `make clql-smoke` |
| no Lua surface | Lua facade/source rock | Lua module parity with current expectations | `make lua-test`, Lua artifact verification |
| no fuzz surface | AFL++ fuzz target and corpus | parser/stream robustness | `make fuzz-smoke` |
| no dependency archive cache contract | shared `CPKT_DEPENDENCY_CACHE` resolution | self-contained build does not download archives; future dependencies have one cache boundary | `make dependency-cache-check`, `make dependency-cache-privacy-regression` |

## Completed migration status

1. Bootlin and AFL++ resolver scripts are wired into CMake toolchain files and
   Make targets.
2. CMake presets are normalized and covered by preset/lifecycle verification.
3. ASan/libFuzzer lifecycle gates are retired in favor of AFL++ smoke and fuzz
   targets through the cached Bootlin x86_64 GNU toolchain.
4. `clql` and executable examples are restored. Selection/count CLI,
   Go-compatible selection flags, AND/OR selector arguments, projection,
   mutation, file-backed mutation values, and fixture smoke are in place.
   Theme/prettyx remains explicitly unsupported.
5. The Lua facade, development rock, Lua tests, and release Lua artifacts are
   restored. The installed `lql.lua` reference executable calls the native
   `lql.core` binding directly and is checked against `clql` and the pinned Go
   CLI for shared workflows.
6. Install rules, CMake package config, pkg-config metadata, and extracted SDK
   consumer tests are in place for install-tree smoke, host binary SDK archive
   verification, and release matrix package verification.
7. Target-tool discovery for package generation and verification covers
   configured CMake cache values, compiler sibling tools, PATH fallback, and
   target mismatch checks. Package verification consumes it for ELF and Mach-O
   loader metadata checks.
8. Package generation, checksum manifest generation, privacy scans,
   relocatability checks, source archives, Lua artifacts, and the release matrix
   are in place. `make release-matrix` covers all Linux Bootlin targets and
   Darwin when osxcross is available.
9. `prerelease` and `release` share the release pipeline; `release` runs the
   version contract first, cleans generated state, and then runs the same proof
   graph.

Future lifecycle gaps should be tracked as ordinary product or release work,
not as unfinished migration items in this ledger.
