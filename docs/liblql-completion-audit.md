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
- `clql-0.0.0-<target>.tar.gz` for:
  - `x86_64-linux-gnu`
  - `x86_64-linux-musl`
  - `aarch64-linux-gnu`
  - `aarch64-linux-musl`
  - `armhf-linux-gnu`
  - `armhf-linux-musl`

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
| Source and Lua release artifacts | `make package-verify`, `make release`, checksum manifest | Proven locally |
| Privacy and relocatability verification | `make package-verify`, `make release`, package privacy fixtures | Proven locally |
| Warning-clean release build with `-Werror` | release-surface CTest in `make test-all` and source archive verification | Proven locally |

## Remaining Unproven Scope

The goal is not complete until these items are resolved or explicitly accepted
as out of scope by the engineer:

1. `arm64-apple-darwin` artifact production is unproven locally.
   `make release-matrix` and `make release` currently report:
   `release-matrix: skipping arm64-apple-darwin: target compiler cannot link`.
   The lifecycle allows optional Darwin skipping when the local cross toolchain
   is unavailable, but the original target matrix still names Darwin as a
   target. A final completion claim needs either a working Darwin toolchain run
   or an explicit release-scope decision that Darwin is optional for this
   release.

2. Final tagged release artifacts are unproven.
   The current worktree is untagged, so generated artifacts intentionally use
   version `0.0.0`. A real release still needs release authority, version
   selection, a lightweight `vX.Y.Z` tag on `HEAD`, clean tagged `make release`,
   and checksum-listed upload selection from the tagged artifact set.

3. Full LQL parity is not claimed by the repository.
   The spec still says full `clql` parity is active porting work and warns not
   to claim full LQL parity until verification proves it. Current parity
   manifests are broad and green, but a final completion claim requires a
   requirement-by-requirement comparison of the Go `pkt.systems/lql v0.17.1`
   behavior against the C SDK, CLI, and Lua facade surfaces, not just passing
   the existing manifests.

4. Release publication is not done.
   No release branch squash, tag push, or GitHub release creation has been
   performed. That is intentionally outside an implementation verification pass
   unless the engineer starts the release flow.

## Next Completion Work

The next non-cosmetic work should be one of:

- provision or point the repository at a working `arm64-apple-darwin` toolchain
  and run `make release-matrix` plus `make release`;
- perform the full requirement-by-requirement parity audit against
  `pkt.systems/lql v0.17.1`, converting any missing behavior into C-native
  implementation and product tests;
- start the formal release flow with release authority, version selection, and
  tagged clean `make release`.
