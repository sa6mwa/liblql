# liblql Completion Audit

This audit records current evidence for the full `liblql` port objective and
the remaining unproven scope. It is not a release note and it is not a claim
that full LQL parity is complete.

## Audit State

- Audit date: 2026-06-26
- Current release version source: untagged git worktree, resolving to `0.0.0`
- Current release command status: clean `make release` passes locally
- Current worktree status at audit creation: clean before this audit document

## Proven Locally

The following gates have passed from the current repository state:

- `make test-all`
- `make bench-check`
- `make bench-memory-check`
- `make package-verify`
- `make release-matrix`
- `make bench-1g-check`
- clean `make release`
- `LQL_PACKAGE_TARGETS=arm64-apple-darwin make release-matrix`

The clean release rehearsal produced and verified:

- `liblql-0.0.0.tar.gz`
- `liblql-lua-0.0.0.tar.gz`
- `liblql-0.0.0-1.rockspec`
- `liblql-0.0.0-1.src.rock`
- `liblql-0.0.0-CHECKSUMS`
- `liblql-0.0.0-<target>.tar.gz` for:
  - `x86_64-linux-gnu`
  - `x86_64-linux-musl`
  - `aarch64-linux-gnu`
  - `aarch64-linux-musl`
  - `armhf-linux-gnu`
  - `armhf-linux-musl`
  - `arm64-apple-darwin`
- `clql-0.0.0-<target>.tar.gz` for:
  - `x86_64-linux-gnu`
  - `x86_64-linux-musl`
  - `aarch64-linux-gnu`
  - `aarch64-linux-musl`
  - `armhf-linux-gnu`
  - `armhf-linux-musl`
  - `arm64-apple-darwin`

The release rehearsal verified the checksum manifest and expanded source
archive test suite. The 1 GiB benchmark gate wrote
`build/bench-1g-check.jsonl` and passed the 128 MiB streaming memory profile.

## Requirement Evidence Map

| Requirement | Current evidence | Status |
| --- | --- | --- |
| C89 public SDK with receiver API | `make test-all`, header C90/C++ tests, public API style gate, SDK manifest gate | Proven locally |
| Per-instance allocator boundary | public API style gate, handle allocator tests, ASan/UBSan in `make test-all` | Proven locally |
| No adjacent Go source references in repository files | repository-boundary CTest in `make test-all` | Proven locally |
| lonejson from GitHub release SDK archives | dependency acquisition in clean `make release`; package dependency manifests verified | Proven locally |
| Selector behavior for claimed scope | C SDK tests with unique manifest requirement keys plus Go-backed `make parity-test` inside `make test-all`; CLI and SDK parity manifests reject duplicate requirement keys | Proven locally for claimed scope |
| Projection behavior for claimed scope | C SDK tests with unique manifest requirement keys, CLI parity tests, SDK parity tests; CLI and SDK parity manifests reject duplicate requirement keys | Proven locally for claimed scope |
| Mutation behavior for claimed scope | C SDK tests with unique manifest requirement keys, CLI parity tests, SDK parity tests; CLI and SDK parity manifests reject duplicate requirement keys | Proven locally for claimed scope |
| Streaming decision and plus-value behavior | C SDK streaming tests, benchmark memory gates, 1 GiB memory gate | Proven locally for claimed scope |
| Lua facade is direct liblql binding, not `clql` backed | Lua C module tests, Lua runtime fixtures, Lua release artifact verification | Proven locally |
| Lua 5.5 only | C compile-time guard, Lua runtime fixtures, Lua package contract fixtures | Proven locally |
| Go/C/Lua benchmark parity and memory gates | `make bench-check`, `make bench-memory-check`, `make bench-1g-check` | Proven locally |
| Linux GNU/musl release artifacts | `make release-matrix`, `make release`, checksum manifest | Proven locally |
| Darwin arm64 release artifacts | `LQL_PACKAGE_TARGETS=arm64-apple-darwin make release-matrix`, checksum manifest | Proven locally with available osxcross toolchain |
| Source and Lua release artifacts | `make package-verify`, `make release`, checksum manifest | Proven locally |
| Release version resolution | `make test` via `lql.release-version` CTest fixture | Proven locally for untagged git, source archive `VERSION`, lightweight tags, annotated-tag rejection, and shell/Make/CMake overrides |
| Privacy and relocatability verification | `make package-verify`, `make release`, package privacy fixtures | Proven locally |
| Warning-clean release build with `-Werror` | release-surface CTest in `make test-all` and source archive verification | Proven locally |

## Remaining Release Work

The implementation scope is locally proven for the current public C/Lua/CLI
contract. Release execution remains unproven until these items are performed
under release authority:

1. Final tagged release artifacts are unproven.
   The current worktree is untagged, so generated artifacts intentionally use
   version `0.0.0`. A real release still needs release authority, version
   selection, a lightweight `vX.Y.Z` tag on `HEAD`, clean tagged `make release`,
   and checksum-listed upload selection from the tagged artifact set.

2. Release publication is not done.
   No release branch squash, tag push, or GitHub release creation has been
   performed. That is intentionally outside an implementation verification pass
   unless the engineer starts the release flow.

## Contract Boundary

Full LQL parity is claimed only for the current public C/Lua/CLI contract. The
executable `parity/oracle_inventory.tsv` classifies every test, benchmark, and
example-bearing file in the pinned Go `pkt.systems/lql v0.17.1` module. Rows
are either covered by C SDK, CLI, Lua, benchmark, or release-gate evidence, or
deliberately marked not-applicable for Go-only API shapes.

The one known Go behavior excluded from the current callback-source contract is
documented as `docs/liblql-dependency-gaps.md`: a non-seekable source
containing root-array items followed by additional top-level values in the same
stream. liblql must not emulate that by materializing the root array or source.

## Next Completion Work

The next non-cosmetic liblql work is the formal release flow: release authority,
version selection, a lightweight `vX.Y.Z` tag on `HEAD`, tagged clean
`make release`, and checksum-listed upload selection.
