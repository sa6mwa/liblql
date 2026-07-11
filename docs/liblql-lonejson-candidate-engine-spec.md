# liblql/LoneJSON Candidate Engine Cutover Specification

## Scope And Status

This is the authority for vendored LoneJSON candidate-engine work. It replaces
the prior hybrid direction, retired candidate CR notes, and any assumption that
additive fast paths are an acceptable end state.

liblql is unreleased. The vendored LoneJSON candidate surface is private to
this repository and has no compatibility obligation. Candidate-engine work,
verification, profiling, and performance acceptance use only the vendored
preset until an upstream LoneJSON release implements the final surface. The
normal preset is not a build, test, or compatibility gate during this work.

The architectural cutover is complete. Candidate Run uses a path-aware
observer and one spill-backed action stage. Decision and payload execution use
the same generic path-event lifecycle; scan plans, specialized candidate
visitors, transformed staging, and payload-side mutation bypasses are deleted.
The remaining work is hardening and performance acceptance on this final path.

## Outcomes

- Candidate streams are strict NDJSON. A root array is a hard error at every
  liblql, clql, SDK, Lua, and benchmark entry point. There is no flattening.
- LoneJSON has one generic Candidate Run traversal: decoded JSON events with
  parser-owned paths delivered to one path-aware observer.
- LoneJSON has one unresolved-output representation: a compact current-
  candidate action stage, bounded in memory and spill-backed when necessary.
- LoneJSON owns JSON mechanics. liblql owns selector, projection, mutation,
  temporal, and public API policy.
- RSS does not grow with total input, candidate count, match count, or result
  count. State belonging to one completed candidate is reset before the next.
- After the complete cutover, every accepted Go/C benchmark row is at least
  1.0x. Improving remaining rows to 1.2x is worthwhile only when it preserves
  this architecture and clear developer experience.

## Final Architecture

The only Candidate Run shape is conceptually:

```text
candidate_run(reader, strict_ndjson, path_observer, output_policy, payload_policy)
```

Its roles are deliberately narrow:

- `reader`: file, callback, and other source adapters converge before parsing.
- `strict_ndjson`: framing and root-array rejection.
- `path_observer`: generic decoded path/value events. LoneJSON never receives
  an LQL selector, predicate, mutation, or temporal rule.
- `output_policy`: pass through, suppress, transform, or stop using the same
  writer lifecycle.
- `payload_policy`: no payload, seekable range metadata, or an explicit
  callback-scoped current-candidate spool.

There are no Candidate Run scan plans, non-path observers, direct visitors,
transform-stage thresholds, selector-shaped fields, or runtime dispatch based
on selector family. A private implementation optimization is permitted only if
it is unobservable, uses the same path-event lifecycle, and does not introduce
another Candidate Run option, state machine, or callback model.

## Candidate Lifecycle

Every candidate follows this lifecycle exactly once:

```text
begin
  -> parse and deliver decoded path events
  -> liblql updates selector truth and transform policy
  -> retain generic writer actions while output commitment is unknown
  -> finalize selector truth and deferred transform errors
  -> accept: commit actions and finish directly
     reject: discard actions and finish validation without output
     stop/fail: propagate the generic result
end
```

LoneJSON parses a source candidate once. Generic projection and mutation do
not reparse raw candidate bytes. The action stage is a compact generic writer
command stream, never selector syntax or liblql transform state. It may spill
only the current candidate; it is reset before the next candidate.

The action codec omits key, string, and number begin markers. Replay opens the
corresponding writer state on its first chunk, or on its end marker for an empty
key or string. This remains a generic parsed-event representation rather than
raw JSON or selector-specific state.

When liblql proves rejection, LoneJSON stops output work but continues JSON
validation and observer delivery. When it proves acceptance, LoneJSON commits
the staged prefix and writes subsequent events directly. If truth remains
unknown until candidate end, it commits or discards there. Deferred mutation
errors remain deferred until the candidate outcome is known.

Callback-source payload delivery may retain a raw current-candidate spool only
for that public callback contract. It is not a transform fallback.

## Ownership Boundary

LoneJSON owns:

- strict NDJSON framing, tokenization, validation, decoded path/value events,
  source offsets, parser/writer stacks, escaping, and output framing;
- bounded parser and writer buffers plus current-candidate action or payload
  spools;
- generic stop, callback, I/O, and JSON diagnostics.

liblql owns:

- selector AST parsing, generic path matching, predicates, temporal rules,
  and final candidate truth;
- projection and mutation semantics, ordering, and error precedence;
- C, CLI, Lua, benchmark, and Go-parity policy.

LoneJSON must not know LQL syntax or Go fixtures. liblql must not tokenize,
parse, serialize, escape, or frame JSON.

## liblql Shape After Cutover

liblql has four execution families only:

1. `candidate_decide`: decision-only scan through the generic path observer.
2. `candidate_payload`: generic path observer plus seekable range metadata or
   explicit callback-scoped current-candidate payload spool.
3. `candidate_run`: projection, mutation, and projection-then-mutation through
   Candidate Run.
4. `buffered_value`: explicitly buffered single-value helper APIs only.

File and callback-source forms differ only in reader or payload policy. They do
not have separate selector, projection, mutation, or transform state machines.

## Required Deletions

The cutover requires deletion, not bypassing:

- `lonejson_candidate_scan_plan`, its stream-option field, scan dispatch, and
  related feature macros; these are deleted;
- `configure_candidate_eval_visitors()` and the `enable_fast_*()` candidate
  selector routes in `src/lql_eval.c`; these are deleted;
- Candidate Run `observer_value`, direct-value visitor state, and all related
  macros and tests; these are deleted from Candidate Run;
- Candidate Run `transform_stage_threshold`, transformed-output spool, and
  hybrid writer reinitialization; these are deleted from Candidate Run;
- retired candidate-output, raw replay, projected replay, compatibility
  aliases, and source/file fallbacks that invoke them;
- tests whose only purpose is preserving removed API behavior.

Replace removed tests with observable behavior tests for the final lifecycle.
Do not retain a fallback for normal presets: the vendored preset proves the
clean implementation until upstream supplies the new surface.

## Allowed State And RSS Invariants

Allowed per execution:

- selector-plan scratch proportional only to selector complexity;
- parser/writer stacks and bounded transport buffers;
- one current-candidate action stage for delayed output;
- one raw current-candidate payload spool only for public callback payload
  delivery.

Forbidden:

- whole-input, whole-stream, all-match, result, selector-result, or candidate
  result caches;
- retaining a completed candidate past its callback or output lifecycle;
- full-message materialization behind a streaming API;
- a second JSON parser or ad hoc JSON rewriting in liblql.

The 8 MiB process target is the architectural target. The 128 MiB limit for
the 100 MiB large-JSON gate is a regression ceiling. The large-candidate RSS
test must prove that repeated spilled candidates do not accumulate RSS. A
multi-candidate test is required because a per-candidate bound alone does not
prove lifecycle reset.

## Large-Slice Protocol

Implement architectural slices before broad verification. Intermediate edits
inside a slice may not compile. Do not preserve an old path merely to keep a
micro-step green.

1. **Candidate-engine cutover.** Delete all alternate Candidate Run observer,
   staging, and scan-plan surfaces. Keep one path observer and one action
   stage.
2. **liblql collapse.** Move decision and payload execution to the same
   path-event lifecycle. Delete fast selector visitors, duplicate scanners,
   adapters, macros, and stale tests in the same slice.
3. **Hardening and performance.** Run semantic, sanitizer, fuzz, RSS, and
   benchmark gates on the completed vendored architecture. Profile and change
   only the final hot path. Pursue 1.2x only through simplifications that keep
   the final ownership boundary clear.

Verification is deliberately sparse during implementation: compile or run one
direct test only to resolve an interface error; run a full vendored suite at
the end of each large executable slice; run the Go/C matrix only once the final
hot path is active. Formatting is part of every slice commit.

## Performance Decision Record

The completed one-parse action-stage implementation is semantically clean but
does not yet meet the performance floor. Its first 4 KiB mutation matrix
recorded 288 Go/C pairs: 188 at or above 1.0x, 100 below, with a 1.13x median
and a 0.65x worst row. Omitting key, string, and number begin actions improved
that to 208 pairs at or above 1.0x, 80 below, and a 1.15x median, but the worst
sparse recursive row was still 0.37x. C peak RSS across those rows was 2.7-3.2
MiB after correcting Linux measurement to read `/proc/self/status` `VmHWM`.

Profiling removed direct spool writes as the dominant cost, but rejected
candidates still require decoded action recording. This cannot be safely
discarded after a direct-path miss: JSON permits duplicate object keys and a
later occurrence of the same path can make the candidate match. The Go
reference retains bounded raw candidate bytes and reparses only accepted
candidates, which avoids that rejected-candidate decoded-action cost. A direct
vendored raw-spool experiment was worse: 99/288 rows at or above 1.0x, a 0.91x
median, and a 0.44x worst row. Copying every raw candidate cost more than the
decoded action stage, so that route is rejected without a first-pass capture
mechanism that avoids per-candidate copying.

Do not reintroduce scan plans, selector-shaped LoneJSON fields, or an
unbounded cache to close this gap. If the one-parse action stage cannot reach
the 1.0x floor after further generic codec work, an explicit revision must
choose a single bounded raw-candidate replay representation for unresolved
output. That revision must preserve strict NDJSON, parser-owned generic path
events, one-candidate lifetime, deferred matched-only mutation errors, and
the repeated-large-candidate RSS proof. It is a material architecture change,
not an implicit optimization under the current specification.

## Architecture Cutover Gates

The architectural cutover is complete only when all of these are true:

- `rg` finds no deleted Candidate Run observer, transformed-stage, scan-plan,
  or fast-candidate selector surfaces outside this migration record;
- no LoneJSON candidate option or macro is selector-family-specific;
- strict NDJSON root-array errors pass in C SDK, CLI, Lua, parity, and
  benchmark fixtures;
- Go/C behavioral parity passes for accepted selector, projection, mutation,
  source, seekable, payload, stop, and error paths;
- the vendored header is a coherent generic candidate surface suitable for
  upstream review;
- the worktree contains no obsolete candidate-engine code, stale tests, or
  contradictory documentation.

## Post-Cutover Acceptance

The performance goal is not part of architectural cutover. It is accepted only
after the final path satisfies all of these gates:

- vendored C suites, sanitizers, and fuzz runs pass;
- 100 MiB and repeated large-candidate RSS gates pass;
- every accepted Go/C benchmark row is at least 1.0x; results identify rows
  below 1.2x without treating them as an architectural reason to add paths;

## Non-Goals

- LQL syntax or semantics in LoneJSON.
- Root-array flattening in liblql or clql candidate streams.
- Cache-based performance fixes.
- A liblql JSON parser, serializer, escaper, or framing layer.
- Backward compatibility for the vendored candidate/transform API.
