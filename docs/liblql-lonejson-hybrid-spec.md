# liblql/LoneJSON Clean-Cut Candidate Engine Specification

## Status And Authority

This is the sole authority for vendored LoneJSON candidate-engine work. It
replaces the previous hybrid-direction document, the retired candidate CR
notes, and any assumption that additive fast paths are an acceptable end
state.

liblql is unreleased. The vendored LoneJSON candidate and transform APIs are
private to this repository and have no compatibility obligation. The normal
presets continue to consume released upstream LoneJSON unchanged. The vendored
preset exists to prove the final design before an upstream handoff.

The hard outcomes are:

- strict NDJSON only; a root array is a hard error at every candidate-stream
  entry point;
- C is at least 1.0x Go on every accepted Go/C benchmark row, with 1.5x the
  operating target;
- RSS is independent of total input size, candidate count, match count, and
  result count; large current candidates spill instead of growing RSS;
- one coherent candidate engine, no compatibility layer, dead executor, or
  selector-specific LoneJSON API surface left behind.

## Current Debt

The current vendored header contains useful performance work but is not the
final design. In particular, it still contains the old generic
`lonejson_candidate_transform_*` executor and gated raw-spool replay path.
liblql still uses that executor for general source mutation and projection.
It must not be described as retired until it is deleted.

The current header also exposes an accumulated set of liblql-shaped knobs:

- top-level string-equality fields;
- top-level multi-field fields;
- recursive literal-field fields;
- direct-path key/kind arrays;
- gated capture and capture-prune callbacks.

Those optimizations may remain temporarily as private implementations, but the
individual knobs are not an acceptable final boundary. They create spread-out
selection logic and make LoneJSON's candidate interface an accidental LQL
execution API.

The recent source-spooled match-decision reuse is a small liblql improvement,
not this clean cut. The large-candidate RSS test is a guardrail, not proof that
the old executor has been removed.

### Live Inventory Baseline

This is the required starting inventory. Update it as migrations delete rows;
do not remove a row merely because a faster sibling path exists.

| Current owner | Live responsibility | Required disposition |
| --- | --- | --- |
| `lonejson_candidate_stream_options` selector fields | Top-level equality/multi-field, recursive-field, direct-path, and capture-prune dispatch | Replace with the one opaque generic plan descriptor; delete fields and feature macros. |
| `configure_candidate_eval_visitors()` and `enable_fast_*()` in `src/lql_eval.c` | Maps LQL selector shapes into LoneJSON selector-shaped option fields | Fold into one liblql plan adapter for the final primitive. |
| `execute_query_source_v2_transform()` | The only external caller of `lonejson_transform_candidates_reader()`; source projection/mutation and projection-then-mutation | Migrate first to staged output, then delete all `source_transform_*` transform-executor glue. |
| `execute_query_file_range_spooled_matches()` | Seekable projection/mutation capture followed by separate projection/mutation work | Migrate to `candidate_output`; delete gated capture as an internal transform fallback. |
| `execute_mutate_source_matches_only_spooled()` | Root-create source-mutation special case that captures then reparses | Delete after staged output supports root creation. |
| `execute_mutate_file_range_candidates_fast()` | Separate seekable mutation scanner/writer path | Fold into `candidate_output` without changing matches-only or unmatched-output semantics. |
| `source_spooled_match_state` payload delivery | Public callback-scoped raw payload handles | Preserve as `candidate_payload`; retain bounded capture only for this public contract. |

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

## Required Deletions

Completion requires deleting, not merely bypassing:

- the existing `lonejson_candidate_transform_*` types, options, aliases,
  transform executor, projection/replay executor, and gated raw-spool replay
  executor;
- `LONEJSON_CANDIDATE_TRANSFORM_MODE_GATED_SPOOLED` and its replay-count/
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
3. `candidate_output`: projection, mutation, and projection-then-mutation
   through the final LoneJSON candidate primitive.
4. `buffered_value`: explicitly buffered single-value helper APIs only.

File and callback-source forms differ only in their reader or payload policy.
They must not have separate selector, projection, mutation, or transform state
machines. A fast path is valid only when it is an internal implementation of
one of these four families and shares its behavior tests.

## Migration Checklist

Work is ordered. Do not claim the clean cut is complete early.

1. Inventory every current liblql call site and LoneJSON symbol using the old
   transform/replay surface. Record its public behavior, output form, and test
   coverage.
2. Specify and implement the one internal candidate primitive in vendored
   LoneJSON, including staged-output and deferred-error semantics.
3. Move direct, recursive, and top-level fast scans behind its opaque plan
   dispatch. Remove their public option fields and feature macros.
4. Migrate `candidate_decide` and `candidate_payload`; prove counters, stops,
   payload lifetime, and root-array errors.
5. Migrate projection and all mutation shapes, including unmatched emission,
   matches-only, increments, removals, creates, wildcards, recursive paths,
   projection-before-mutation, and malformed/read-error partial results.
6. Delete the old transform/replay executor and all callers in the same change
   sequence. A build must fail if an old symbol remains referenced.
7. Simplify liblql to the four execution families. Delete duplicate adapters,
   state structs, and tests for deleted internals.
8. Run the gates below and write the upstream LoneJSON handoff specification.
9. Upstream the final generic candidate surface. After an upstream release,
   switch normal presets to it and prove that semantics, RSS, and performance
   remain intact.

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
- every accepted Go/C benchmark row is at least 1.0x, and the result records
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
