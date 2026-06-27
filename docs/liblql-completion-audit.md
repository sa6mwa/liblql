# liblql Completion Audit

This audit records current evidence for the full `liblql` port objective and
the remaining unproven scope. It is not a release note and it is not a claim
that full LQL parity is complete.

## Audit State

- Audit date: 2026-06-27
- Implementation evidence commit: `d950dc4 fix(selector): align temporal format parity`
- Current release version source: untagged git worktree, resolving to `0.0.0`
- Current release command status: stale; do not treat previous release rehearsal
  as final after the parity reassessment
- Current worktree status at audit update: reassessment changes in progress

## Reassessment Warning

This repository is no longer considered complete. The temporal selector audit
found a real Go/C divergence that the previous parity evidence did not catch:
C accepted leap-second timestamps while Go rejected them. That specific bug is
fixed, but the failure mode shows that prior broad `covered` claims were too
coarse.

See `docs/liblql-parity-reassessment.md`. Until every `partial` oracle row has
explicit matrix evidence or a narrowed non-applicable boundary, this document is
only historical evidence of gates that have passed, not a completion
certificate.

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
archive test suite. The focused C/Lua 1 GiB benchmark gate writes
`build/bench-1g-check.jsonl`, checks exact generated counts for the selected
streaming modes, and applies the 128 MiB memory profile.

## Requirement Evidence Map

| Requirement | Current evidence | Status |
| --- | --- | --- |
| C89 public SDK with receiver API | `make test-all`, header C90/C++ tests, public API style gate, SDK manifest gate | Proven locally |
| Per-instance allocator boundary | public API style gate, handle allocator tests, ASan/UBSan in `make test-all` | Proven locally |
| No adjacent Go source references in repository files | repository-boundary CTest in `make test-all` | Proven locally |
| lonejson from GitHub release SDK archives | dependency acquisition in clean `make release`; package dependency manifests verified | Proven locally |
| Selector behavior for claimed scope | C SDK tests with unique manifest requirement keys plus Go-backed `make parity-test`; temporal format matrix now exists, but other selector families require reassessment | Partial pending matrix audit |
| Projection behavior for claimed scope | C SDK tests with unique manifest requirement keys, CLI parity tests, SDK parity tests; exact matrix breadth requires reassessment | Partial pending matrix audit |
| Mutation behavior for claimed scope | C SDK tests with unique manifest requirement keys, CLI parity tests, SDK parity tests; exact matrix breadth requires reassessment | Partial pending matrix audit |
| Streaming decision and plus-value behavior | C SDK streaming tests, benchmark memory gates, 1 GiB memory gate; exact matrix breadth requires reassessment | Partial pending matrix audit |
| Lua facade is direct liblql binding, not `clql` backed | Lua C module tests, Lua runtime fixtures, Lua release artifact verification | Proven locally |
| Lua 5.5 only | C compile-time guard, Lua runtime fixtures, Lua package contract fixtures | Proven locally |
| Go/C/Lua benchmark parity and memory gates | `make bench-check` and `make bench-memory-check` for Go-backed parity; `make bench-1g-check` for focused C/Lua 1 GiB bounded-memory invariants; clean `make release` | Proven locally |
| Linux GNU/musl release artifacts | `make release-matrix`, `make release`, checksum manifest | Proven locally |
| Darwin arm64 release artifacts | `LQL_PACKAGE_TARGETS=arm64-apple-darwin make release-matrix`, checksum manifest | Proven locally with available osxcross toolchain |
| Darwin x86_64 release artifacts | lonejson `v0.35.0` GitHub release asset inventory has no `liblonejson-0.35.0-x86_64-apple-darwin.tar.gz`; liblql dependency policy requires GitHub release SDK archives | Not a current package target |
| Source and Lua release artifacts | `make package-verify`, `make release`, checksum manifest | Proven locally |
| Release version resolution | `make test` via `lql.release-version` and `lql.package-version-fixtures` CTest fixtures | Proven locally for untagged git, source archive `VERSION`, lightweight tags, annotated-tag rejection, shell/Make/CMake overrides, source/Lua package override names, embedded `VERSION`, release rockspec metadata, and checksum manifest naming |
| Privacy and relocatability verification | `make package-verify`, `make release`, package privacy fixtures | Proven locally |
| Warning-clean release build with `-Werror` | release-surface CTest in `make test-all` and source archive verification | Proven locally |

## Remaining Implementation And Release Work

The implementation scope is not yet proven. Before release, the parity
reassessment must be completed and every `partial` oracle row must be resolved.
Release execution also remains unproven until these items are performed under
release authority:

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

The next non-cosmetic liblql work is parity proof repair, not release. Start
with the high-risk surfaces in `docs/liblql-parity-reassessment.md`, add or
tighten Go-vs-C SDK/CLI matrices, downgrade any unproven inventory rows to
`partial`, and only then return to release authority, version selection, tagged
clean `make release`, and checksum-listed upload selection.
