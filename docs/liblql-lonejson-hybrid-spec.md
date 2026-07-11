# liblql/LoneJSON Clean-Cut Candidate Engine Specification

## Status And Authority

This is the sole authority for vendored LoneJSON candidate-engine work. It
replaces the previous hybrid-direction document, the retired candidate CR
notes, and any assumption that additive fast paths are an acceptable end
state.

liblql is unreleased. The vendored LoneJSON candidate and transform APIs are
private to this repository and have no compatibility obligation. The normal
presets continue to consume released upstream LoneJSON unchanged. The vendored
preset exists to prove the final design before an upstream handoff. Until that
handoff, candidate-engine development, verification, profiling, and performance
acceptance use the vendored preset exclusively. The normal preset is neither a
compatibility target nor a required build or test gate for this implementation.

The hard outcomes are:

- strict NDJSON only; a root array is a hard error at every candidate-stream
  entry point;
- C is at least 1.2x Go on every accepted Go/C benchmark row, with 1.5x the
  operating target;
- RSS is independent of total input size, candidate count, match count, and
  result count; large current candidates spill instead of growing RSS;
- one coherent candidate engine, no compatibility layer, dead executor, or
  selector-specific LoneJSON API surface left behind.

## Current Implementation And Debt

The vendored path now has one `lonejson_candidate_run_*` executor. It parses a
candidate once, observes original decoded events, records a spill-backed
compact writer-action prefix while selection is unknown, commits that prefix
without reparsing on acceptance, and discards it on rejection. Source and
seekable projection/mutation use this executor. The former raw-source replay,
projected replay, output modes, decision callbacks, compatibility adapter, and
separate liblql file mutation executor have been deleted.

The remaining architecture debt is narrower but still material:

- stale candidate scan accelerators and aliases must be inventoried and
  collapsed behind one generic plan surface;
- Candidate Run uses direct-value staging for eligible fast selectors, but
  generic selector shapes still traverse the path-visitor callback stack;
- projection synthesis remains interleaved with the transform writer and needs
  a final dead-code audit;
- completion terminology and documentation still contain historical
  `candidate_output` and parser-replay wording that must be removed;
- the complete parity, sanitizer, fuzz, RSS, and performance matrices have not
  run against the final code because the architecture/performance slice is
  still open.

### Live Inventory Baseline

This is the required starting inventory. Update it as migrations delete rows;
do not remove a row merely because a faster sibling path exists.

| Current owner | Live responsibility | Required disposition |
| --- | --- | --- |
| `lonejson_candidate_scan_plan` | Vendored generic JSON scan descriptor now owns object-member, descendant-member, path, and string-equality acceleration. Normal presets use a narrow source adapter until upstream ships the plan. | Delete the upstream-field adapter and the old feature macros when the upstream handoff lands; retain private scanner dispatch only. |
| `configure_candidate_eval_visitors()` and `enable_fast_*()` in `src/lql_eval.c` | Maps LQL selector shapes into LoneJSON selector-shaped option fields | Fold into one liblql plan adapter for the final primitive. |
| `execute_query_source_candidate_run()` | Sole liblql projection/mutation executor for source and seekable readers | Collapse remaining liblql transform matching into a compiled generic traversal plan, then minimize glue. |
| Candidate Run action stage | Compact opcode stream with spill-backed per-candidate storage and no source/path replay metadata | Retain; profile command production/commit and prove repeated-large-candidate RSS. |
| `source_spooled_match_state` payload delivery | Public callback-scoped raw payload handles | Preserve as `candidate_payload`; retain bounded capture only for this public contract. |

## Execution Correction

The first implementation attempt did not cut over to this architecture. It
renamed the transform executor, added staged-output and selector-specific fast
paths around it, and repeatedly verified those increments while the gated raw
capture, parser replay, projected replay, compatibility aliases, and duplicate
liblql execution paths remained authoritative. Source mutation consequently
continued to serialize rejected candidates and measured only 0.37x to 0.57x
of Go on the representative steady-state rows.

That failure establishes these implementation rules:

- A rename, adapter, sibling fast path, or bypass does not count as migration.
  Progress requires replacing ownership and deleting the superseded path.
- Do not optimize a legacy executor scheduled for deletion. Performance work
  belongs inside the final primitive after the complete hot path uses it.
- Do not begin at one liblql call site. Build the generic LoneJSON lifecycle
  and candidate-specific action stage first, then migrate all callers in one
  cutover slice.
- Do not retrofit delayed candidate output into the global LoneJSON writer.
  Ordinary JSON writing must not acquire candidate spools, action modes, or
  candidate lifecycle branches. The action stage is private state owned by
  `candidate_run` and exposes only generic JSON writer commands.
- Do not use speculative full-candidate serialization as selection deferral.
  While truth is unknown, record bounded generic writer actions. On accept,
  commit the prefix once and continue directly; on reject, discard it and
  continue validation without output work.
- Do not preserve the vendored legacy API for compatibility. The normal preset
  may continue using released upstream LoneJSON until handoff, but it must not
  constrain or alias the vendored implementation.
- Do not commit additive architecture. A cutover commit must remove the old
  caller, branch, state, or executor it replaces. Temporary code may exist
  inside an unfinished large slice, not as a sequence of permanent siblings.

### Current Performance Assessment

The small 128-candidate diagnostic fixture previously measured about 0.595 ms
for C and 0.576 ms for Go on `eq_status_open/mutate_source_selector`. After
compact action commits and writer changes, the 4 KiB-per-record diagnostic
measures C at 1.07 ms versus Go at 1.23 ms for equality, and C at 1.50 ms
versus Go at 2.01 ms for all-match `contains` under the vendored native bench
preset. These are diagnostic measurements, not acceptance results: the full
accepted matrix remains below the required 1.2x in multiple rows.

The profile and rejected experiments establish the next work:

- compacting delayed commands from a legacy 24-byte header and dead path
  payload to one-byte structural opcodes improved the row by roughly 5%;
- in-memory delayed stages now commit directly from the bounded spool buffer;
  spilled stages keep the streaming decoder, preserving the candidate RSS
  bound;
- commitment is polled after decoded chunks as well as completed values, so a
  monotonic streaming predicate can stop staging as soon as it proves a match;
- enabling fast non-path selector observation during staged mutation preserved
  behavior but did not remove the dominant transform/parser cost;
- polling output commitment only at complete-value boundaries and filtering
  unused insertion phases are correct simplifications but were not measurable;
- replacing the parser's native path visitor with Candidate Run's reconstructed
  traversal stack regressed the row to about 0.62 ms and must not be retained;
- LTO was neutral on this row, so compiler configuration is not the missing
  architectural speedup;
- Go's mutation path pipelines selection and mutation parsers in separate
  goroutines. C remains single-pass and single-threaded, so its remaining loss
  is generic callback, path, staging, and writer work rather than a second
  parse.
- A completed Go/C source-mutation run on the 4 KiB-per-record matrix passed
  observable parity, but is far from the performance floor: the worst
  pretty/nested sparse row was 0.369x (C 1.93 ms, Go 0.71 ms). Most sparse,
  recursive, range, and nested rows remain below 1.0x; equality and selected
  lockd rows are faster than Go. The two direct diagnostic wins do not
  generalize to the accepted matrix.
- The pinned Go reference rejects exists at parse time. Direct liblql coverage
  retains the feature, while the Go/C/Lua performance matrix excludes it until
  the reference supports the common expression.
- Direct-value staging now passes the vendored unit and allocator suites plus
  the full 4 KiB Go/C mutation matrix. It eliminates generic path construction
  for rejected fast-selector candidates and raised sparse equality rows above
  the floor, but generic path selectors and array traversal still miss it;
  the lowest observed row is now 0.401x.

The next implementation slice must compile the remaining common selector
shapes into Candidate Run traversal state so they do not require generic
parser-owned path construction. It must preserve path, wildcard, recursive,
and array semantics without adding selector syntax, cache candidate decisions,
duplicate traversal stacks, or weaken spill-backed RSS bounds.

### Current Hot Path

The direct-value profile of sparse array selection shows that generic path
reconstruction is no longer the dominant cost in eligible rows. JSON value and
string parsing consume about 23% of cycles and compact action staging consumes
about 12%; path reconstruction is about 4.5% and occurs only for accepted
prefixes. The candidate engine must therefore reduce whole-event staging work
for unresolved candidates as well as compiling generic selector traversal.
Changing only mutation path matching or the accepted-prefix traversal cannot
reach the 1.2x floor.

For selectors that resolve only at a candidate's final field, the action stage
still dispatches the candidate's decoded events once to record actions and once
to commit them. Candidate Run now has a bounded hybrid representation for this
case: action staging remains the default, while liblql selector-gated mutation
switches after 1 KiB at a completed scalar/container boundary to a spill-backed
transformed-output stage without reparsing source bytes. The switch never occurs
inside an open streamed scalar. Mutation errors remain deferred until the
selection result and early-rejected candidates remain action-only.

### Large-Slice Development Protocol

Implementation proceeds in three large slices. Within a slice, temporary
compile breakage and incomplete internal wiring are acceptable. The objective
is architectural completion of the slice, not keeping every intermediate edit
green.

1. **Candidate-engine cutover.** Implement `candidate_run`, its spill-backed
   candidate-specific writer-action stage, strict NDJSON lifecycle, plan
   observer, output policy, payload policy, and deferred errors. Migrate source
   and seekable projection/mutation, including matches-only, unmatched output,
   and projection-then-mutation. Delete `lonejson_candidate_output_*`, gated
   raw transform replay, projected replay, and their aliases in the same
   slice.
2. **liblql collapse.** Move decision and payload execution onto the same
   lifecycle, collapse file/source differences into reader and payload
   policies, retain only the four execution families, and delete duplicate
   scanners, adapters, feature macros, state machines, and stale tests.
3. **Hardening and performance.** Run complete semantic, sanitizer, fuzz, RSS,
   and benchmark gates. Profile only final hot paths, implement improvements
   inside the final ownership boundary, and repeat the complete gates after
   the >=1.2x requirement is met.

Verification is deliberately sparse during implementation:

- use compilation or one directly relevant executable only when needed to
  resolve an interface or lifecycle error;
- do not run normal and vendored full suites after individual functions,
  fields, or call-site edits;
- run focused behavior tests when a large slice first becomes executable;
- run the full suite once at the end of each large slice;
- run the benchmark matrix only after the complete final hot path is active;
- run formatting immediately before a slice commit so formatting changes are
  committed with that slice.

Correctness and RSS invariants are maintained structurally while coding:
one input parse, current-candidate-only state, spill-backed stages, reset before
the next candidate, strict root-array rejection, no hidden whole-input storage,
and no second parser. Tests prove those invariants at slice boundaries; they do
not substitute for designing them into the implementation.

## Ownership

LoneJSON owns only JSON mechanics:

- strict NDJSON framing and root-array rejection requested by the caller;
- tokenization, validation, decoded key/scalar delivery, and source offsets;
- parser and writer stacks, escaping, commas, structural validity, and output
  framing;
- bounded read/write buffers and bounded current-candidate spools;
- raw or transformed candidate spool lifecycle and spill-to-disk mechanics;
- generic stop, callback, I/O, and JSON diagnostics.

liblql owns only LQL mechanics:

- selector AST parsing, planning, path matching, predicates, temporal rules,
  and final match truth;
- projection and mutation semantics, ordering, and error precedence;
- public C, CLI, and Lua policy; benchmark acceptance; and Go parity policy.

LoneJSON must not know selector syntax, mutation syntax, LQL date behavior, or
Go parity fixtures. liblql must not tokenize JSON, serialize JSON, escape JSON,
or reimplement NDJSON framing.

## Final Boundary

There is one internal LoneJSON candidate primitive, conceptually:

```text
candidate_run(reader, framing, plan-observer, output-policy, payload-policy)
```

Its concrete C names may differ, but it has exactly these roles:

- `reader`: reader/file/fd/path adapters converge before parsing;
- `framing`: strict NDJSON for liblql, including root-array hard error;
- `plan-observer`: generic decoded JSON event delivery plus optional opaque
  acceleration hints; LoneJSON never interprets LQL meaning;
- `output-policy`: pass through, suppress, staged transformed output, or stop;
- `payload-policy`: no payload, seekable range metadata, or explicit
  callback-scoped current-candidate spool.

There must be one event model and one candidate lifecycle. Fast top-level,
recursive, and direct-path scans are permitted only as private dispatch choices
behind that primitive. liblql supplies one opaque observer/plan descriptor,
not separate `top_level_*`, `recursive_*`, or `direct_path_*` option fields.

The public-upstreamable LoneJSON surface must be generic JSON vocabulary:
candidate metadata, decoded path/value events, writer actions, current-candidate
spools, and stop/error results. It must not expose a field that exists only for
one liblql selector family.

## Required Candidate Lifecycle

Every candidate follows this lifecycle exactly once:

```text
begin
  -> parse and deliver decoded events
  -> liblql updates selector truth and optional transform state
  -> finalize selector truth and deferred transform errors
  -> commit one of: emit original, emit staged transform, emit payload, discard,
     stop, or fail
end
```

The parser reads a source candidate once. Generic projection and mutation must
not reparse captured raw candidate bytes merely because selection completes at
candidate end.

Selection-gated output uses an explicit bounded current-candidate stage:

- For `matches_only`, LoneJSON writes speculative transformed output to a
  current-candidate spool while liblql observes the same parse. At finalization,
  it emits or discards that spool. Mutation errors are deferred until the
  selection result is known, so an unmatched candidate cannot fail a query.
- For modes that must emit unmatched candidates, the engine uses an explicit
  staged original/transformed representation selected at finalization. The
  representation and output spelling must be covered by Go parity tests; it
  may use bounded disk-backed current-candidate staging, but may not replay the
  source through a second parser.
- Public callback-source payload APIs retain a raw current-candidate spool only
  for callback delivery. Their bounded capture is a public contract, not a
  hidden transform fallback.

If a required operation cannot satisfy those rules, it is unsupported until the
candidate engine supports it. It must not silently route to the old replay
executor, a second parser, a whole-input spool, or a liblql JSON writer.

### Delayed Output Requirement

The final primitive must not make speculative full-candidate serialization the
only way to defer selection. That design is semantically valid but loses to Go
when most candidates are rejected: it writes every rejected candidate through
the transform writer before discarding it.

Candidate parsing therefore has a generic delayed-output state with these
properties:

1. LoneJSON delivers decoded events to liblql while retaining only the
   current candidate's bounded output/event prefix needed before a commit
   decision.
2. When liblql can prove a candidate rejected, LoneJSON stops producing
   transformed output for its remaining events but continues JSON validation
   and observer delivery.
3. When liblql can prove a candidate accepted, LoneJSON commits the staged
   prefix and writes the remaining events directly through the selected output
   policy.
4. When truth remains unknown until candidate end, LoneJSON commits or
   discards the bounded stage there. It does not invoke a second JSON parser.

The stage is LoneJSON-owned and generic. It records JSON writer actions or an
equivalent decoded event representation, never selector syntax or liblql
mutation state. Its implementation may use a bounded spillable current-
candidate spool; it must not retain output from a previous candidate.

This is the required replacement for the current source-only exact/multi
early-discard helpers. Those helpers are interim measurements and must be
deleted once the primitive exposes generic `accept`, `reject`, `unknown`, and
`stop` output-policy transitions.

## Required Deletions

Completion requires deleting, not merely bypassing:

- the retired `lonejson_candidate_output_*` types, options, aliases,
  output executor, projection/replay executor, and gated raw-spool replay
  executor;
- `LONEJSON_CANDIDATE_RUN_MODE_GATED_SPOOLED` and its replay-count/
  projected-replay bookkeeping;
- public or semi-public LoneJSON option fields dedicated to top-level equality,
  top-level multi-field, recursive-field, or direct-path LQL acceleration;
- liblql source/file mutation and projection fallbacks that invoke the removed
  executor or parse a retained candidate a second time;
- feature macros and compatibility shims that preserve either deleted surface.

Delete tests whose sole purpose is preserving removed API behavior. Replace
them with behavior tests for the final primitive. Do not retain a fallback for
normal presets: normal presets use the released upstream package, while the
vendored preset carries the clean implementation until upstream ships it.

## Allowed State And Memory

Allowed per receiver:

- selector-plan scratch proportional to selector/plan complexity;
- parser/writer stacks and bounded chunk buffers;
- one current-candidate raw payload spool only when payload delivery requires
  it;
- one or, where unmatched output requires it, two current-candidate output
  stages that spill at the configured threshold and are reset before the next
  candidate.

Forbidden:

- whole-input, whole-stream, all-match, or result caches;
- persistent candidate-result caches or selector-result caches;
- retaining a previous candidate after its callback/commit lifecycle ends;
- a hidden temporary file that changes a streaming API into whole-message
  materialization;
- a second JSON parser or ad hoc JSON rewriting in liblql.

The 8 MiB process target is the architecture target. The 128 MiB 100 MiB gate
is only a regression ceiling. The dedicated large-candidate test must continue
to prove multiple spilled candidates do not accumulate RSS.

## liblql Shape After Cutover

liblql has four execution families only:

1. `candidate_decide`: decision-only scan with no capture.
2. `candidate_payload`: seekable range metadata or explicit callback-scoped
   current-candidate spool.
3. `candidate_run`: projection, mutation, and projection-then-mutation
   through the final LoneJSON candidate primitive.
4. `buffered_value`: explicitly buffered single-value helper APIs only.

File and callback-source forms differ only in their reader or payload policy.
They must not have separate selector, projection, mutation, or transform state
machines. A fast path is valid only when it is an internal implementation of
one of these four families and shares its behavior tests.

## Migration Checklist

The three large slices above are the migration unit. This checklist is an exit
check, not an instruction to create separately verified micro-commits.

- Candidate-engine slice: all output and payload shapes use `candidate_run`;
  old output/transform/replay symbols, aliases, modes, and callers are deleted.
- liblql-collapse slice: only the four execution families remain; scanner and
  selector acceleration is behind one opaque plan adapter.
- Hardening slice: all completion gates below pass and the upstream LoneJSON
  handoff specification describes only the final generic surface.
- Upstream release: replace the normal-preset adapter with the released API and
  rerun semantic, RSS, and performance gates without vendored exceptions.

## Completion Gates

The clean cut is not complete until all of these are true:

- `rg` finds no old candidate-transform/replay symbols outside migration notes;
- no LoneJSON candidate option or macro is selector-family-specific;
- strict NDJSON root-array hard errors pass in C SDK, CLI, Lua, parity, and
  benchmark fixtures;
- Go/C behavioral parity passes for accepted selector, projection, mutation,
  source, seekable, payload, stop, and error paths;
- normal and vendored C suites pass; sanitizer and fuzz runs pass for the
  vendored implementation;
- 100 MiB and large-candidate RSS gates pass, including repeated spilled
  callback-source candidates;
- every accepted Go/C benchmark row is at least 1.2x, and the result records
  all remaining rows below 1.5x with a concrete owner;
- the vendored header contains one coherent generic candidate surface suitable
  for upstream review, not a set of liblql-shaped performance hooks;
- the worktree has no obsolete candidate-engine code, stale documentation, or
  compatibility scaffolding.

## Non-Goals

- LQL syntax or semantics in LoneJSON.
- Root-array flattening in liblql or clql candidate streams.
- Cache-based performance fixes.
- A liblql JSON parser, serializer, escaper, or framing layer.
- Preserving the current vendored candidate/transform API for any consumer.
