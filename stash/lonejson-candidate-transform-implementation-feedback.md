# LoneJSON Candidate Transform Implementation Feedback

This is feedback on `stash/candidate-transform-implementation.md` from the
liblql cutover perspective.

## Summary

The implementation contract has the right shape for liblql. It addresses the
hard blocker found during the earlier direct V2 cutover attempt: liblql needs a
finalized per-candidate policy after first-pass selector observation and before
any replay/output transform callback can mutate or emit the candidate.

The contract now gives liblql that boundary:

- `gated_spooled` mode observes a complete logical candidate before output;
- `candidate_decision` runs after first-pass observation and before replay;
- `DROP` skips replay and emits no bytes for the candidate;
- `EMIT` replays through the same lonejson transform executor;
- the caller-owned `candidate_policy` is visible from the replay root callback
  onward;
- replay does not call `observer`, preventing selector state from being counted
  twice;
- recursive root-array logical candidates get independent decision, policy,
  replay, metadata, and limit behavior.

This should let liblql delete its callback-source replay/staging machinery
instead of replacing it with another liblql-owned replay path.

## What Looks Correct

The explicit mode model is correct:

- `streaming` for plans whose output can be committed as parsing proceeds;
- `gated_spooled` for selector-dependent or otherwise late-decision plans;
- `unsupported` for valid requests that cannot be satisfied without violating
  the selected mode contract.

The `gated_spooled` semantics are the important part. They solve the sparse
late-match failure case:

```json
{"big":"...large...", "id":"a", "status":"open"}
```

For a selector such as `/status = "open"` with a mutation of `/id = "b"` and
`matches_only=true`, earlier semantic content cannot be emitted before selector
truth is known, but it must still be available if the candidate matches. The
document correctly names this as spooled replay rather than pretending the
operation is pure streaming.

The policy pointer lifetime is also suitable for liblql. Because callbacks are
serialized by logical candidate and the pointer is not retained after the
candidate completes, liblql can keep a compact current-candidate policy in its
per-call transform state. It does not need per-candidate allocation, full
candidate bytes, full output bytes, or a replay cache.

The distinction between source coordinates and replay/spool coordinates is
correct. Public metadata should remain source-relative when known and explicit
unknown otherwise. It should never expose temporary spool-relative offsets as
source offsets.

The counter set is useful and testable. In particular, dropped-before-replay and
replayed counts give liblql a way to prove that sparse `matches_only=true`
candidates are discarded before transform callbacks and output.

## Remaining Cutover Gap

The remaining material gap is projection plus mutation composition.

The document currently says projection paths and transform callbacks compose in
one executor over the source-value event stream, while lonejson owns projected
structure emission. That can work for many source-backed paths, but it is not
yet a complete contract for all liblql projected mutation behavior.

The unresolved cases are:

- mutation into an object that exists only because projection synthesized it;
- insertion into projection-synthesized parent objects;
- mutation semantics when projection-before-mutation is intended to operate on
  the projected intermediate shape rather than the original source shape;
- sparse array placeholder synthesis combined with mutation or insertion;
- rejecting unsupported projection/mutation combinations before partial output
  has leaked.

This is not the same blocker as finalized candidate policy. The finalized
candidate policy issue is solved by the current contract. Projection/mutation
composition is the next area that must be made exact before declaring the full
liblql cutover complete.

## Recommended Contract Tightening

Before liblql commits to a total cutover, lonejson should make these points
explicit:

1. Callback ordering for projected output plus transform callbacks.

   Define when transform, replace, and insert callbacks fire relative to
   projected parent synthesis, sparse array placeholder emission, and source
   value traversal.

2. Insertion behavior for synthesized projection parents.

   State whether insertion callbacks fire for lonejson-synthesized objects, not
   only source objects. If they do not, specify which projected mutation shapes
   must return `LONEJSON_STATUS_UNSUPPORTED`.

3. Parent/container metadata.

   liblql can reconstruct root/object-member/array-element relationships from
   path visitor state for now, but this must be reliable enough to test. A
   first-class parent relationship field would reduce downstream state and
   remove ambiguity.

4. Unsupported-before-output guarantee.

   Unsupported projection/mutation compositions must fail before invalid partial
   output is written, especially in streaming mode.

5. Old scalar materialization scope.

   Complete old scalar capture should remain opt-in and ideally path/need
   scoped. If the initial implementation only supports a broad per-call
   materialization toggle, liblql can tolerate that as an implementation
   tradeoff, but it should be visible in mode/options and tested for large-value
   behavior.

## liblql Integration Assessment

Assuming the implementation matches the document, liblql should be able to route
callback-source candidate mutation and projected mutation through
`lonejson_transform_candidates_reader()`.

The liblql state model should be:

- one transform state per call;
- first-pass observer feeds selector evaluation and any counters needed for
  policy;
- `candidate_decision` computes a compact current-candidate policy;
- replay transform callbacks use `event->candidate_policy` from root onward;
- unmatched `matches_only=true` candidates return `DROP`;
- unmatched `matches_only=false` candidates return `EMIT` with mutation and
  insertion disabled;
- matched candidates return `EMIT` with the required projection/mutation policy;
- selector truth is not recomputed from replay-local early state.

For the first clean cutover, liblql should conservatively choose
`gated_spooled` for selector-backed callback-source transforms until individual
plans are proven stream-commit-safe. That avoids leaking output before late
selector truth is known.

## Verdict

This contract is sufficient to proceed with the liblql cutover for the
finalized-candidate-policy problem.

The only remaining architecture-level concern is projection plus mutation
composition. If lonejson intends this implementation to replace all liblql
callback-source replay and projection staging paths, that composition contract
must either be completed now or the affected shapes must be reported as
`LONEJSON_STATUS_UNSUPPORTED` before output starts.
