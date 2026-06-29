# liblql Port Specification

## Goal

Port `pkt.systems/lql v0.17.1` from Go to C89 in this repository as
`liblql`, with a companion CLI named `clql`.

The C port must be suitable for lockd-style local JSON search and mutation
workloads. It must provide feature parity with the Go implementation's public
library, CLI, and supported embedding surfaces while remaining an idiomatic C
library. Parity does not mean transliterating Go structs or implementation
techniques into C. It means liblql exposes the same product capabilities in
C-native forms: receiver objects, explicit ownership, allocator-aware handles,
borrowed views, and stable method tables where Go uses public values and
methods.

The Go implementation is the semantic oracle for convergence. Native C tests
prove the C product contract. Lua tests prove the Lua facade contract. `lonejson`
owns JSON parsing, serialization, validation, stream framing, escaping, payload
capture, and rewrite support.

The implementation must not reference the adjacent Go source checkout from
repository files. Go oracle checks must go through the `parity/` Go module,
which pins `pkt.systems/lql v0.17.1`. Repository files must not derive
behavior from, or reference, an adjacent Go source checkout.

## Parity Contract

The reference Go package exposes LQL as a library, not only as a command-line
filter. liblql must therefore preserve library features, not merely observable
CLI output. A Go public value shape can map to a different C public shape when
that C shape is more idiomatic, but it cannot disappear.

Selector parsing is the clearest example:

- Go parses textual LQL selector expressions into a public recursive
  `Selector` AST.
- Go users can inspect and construct selector AST values directly.
- Go can marshal and unmarshal selector AST values as JSON.

The C/Lua port must expose equivalent capabilities. It may use receiver-style
objects, borrowed node views, builders, visitors, and explicit destroy methods
instead of public mutable Go structs, but selector AST access is not optional.
An opaque selector handle that can only be evaluated is not feature parity.

Implementation guidance:

- Public C APIs should follow C idioms: stable receiver structs, enum tags,
  out-parameters, borrowed `const char *` views with lengths where needed, and
  caller-visible ownership rules.
- Implementation internals may keep optimized compiled plans, finalized selector
  metadata, or streaming execution traits, but those are below the public AST
  contract. These internals must reduce repeated parsing, allocation, or branch
  work; they must not become candidate/result caches or a parallel public AST.
- Lua should expose the same high-value AST capabilities as idiomatic Lua
  userdata backed by the public C API. Table conversion helpers are optional
  convenience facades; Lua must not own a second AST representation, duplicate
  parser logic, or shell out to `clql`.
- Go-only implementation details such as private caches, Go reflection helpers,
  or Go-specific `time.Time` values do not need literal C equivalents. Public
  library features do need equivalents.

## Release Artifacts

The repository produces two artifact families:

1. `liblql`
   - pkt.systems lifecycle-compatible C SDK tarballs;
   - static and shared C libraries where supported;
   - public headers under `include/lql/`;
   - CMake package config and pkg-config metadata;
   - license, README, examples, and dependency provenance.

2. `clql`
   - CLI tarballs named `clql-<VERSION>-<TARGET>.tar.gz`;
   - equivalent to the Go `lql` binary for supported behavior;
   - intentionally does not reimplement `pkt.systems/prettyx` colorized JSON.

Target matrix follows the pkt.systems lifecycle:

- `x86_64-linux-gnu`
- `x86_64-linux-musl`
- `aarch64-linux-gnu`
- `aarch64-linux-musl`
- `armhf-linux-gnu`
- `armhf-linux-musl`
- `arm64-apple-darwin` when the Darwin toolchain is available

## Dependency Boundary

`lonejson v0.35.2` or newer is the JSON substrate. liblql obtains lonejson
from the official GitHub release SDK archives for each target and verifies the
archive SHA-256 before installing it into the local dependency cache.

liblql must not implement bespoke JSON parsing, tokenization, escaping,
serialization, stream framing, compacting, or payload spooling when lonejson can
own that behavior.

If a required JSON capability is missing from the available lonejson release,
liblql must treat that as an external dependency capability gap. It must not
hide an alternate JSON implementation inside liblql.

## Public C API Intent

The public API is C89-compatible and installed under `include/lql/`.

The primary public C API is receiver-function based. Callers create an
instantiatable `lql *` with `lql_new()`, invoke operations as
`ctx->operation(ctx, ...)`, and release it with `ctx->destroy(ctx)`. Selector,
query, payload, projection, compact, mutation, and receiver cleanup operations
are receiver methods only; standalone public functions are limited to
construction and diagnostics.
Project-owned code must also use receiver calls directly rather than recreating
removed operation free functions or free-operation cleanup aliases through local
macros or static wrapper shims.

The API must be handle-oriented and explicit about ownership:

- parsed selector AST handles are owned by the caller and destroyed with
  receiver cleanup methods;
- optimized selector plans, when present, are derived execution artifacts and
  must not replace the public selector AST surface;
- selector, projection, and mutation plan handles are receiver-owned child
  objects: they do not store allocator pointers or provide a fallback cleanup
  domain, and callers must destroy them through the same `lql *` receiver API
  family that created them;
- result payload handles are valid only for documented callback lifetimes and
  must not imply retained candidate copies;
- error messages are actionable and available through explicit error objects.

All liblql-owned allocation must pass through the active receiver instance's
allocator. The allocator is receiver-owned state, not a process-global central
allocator: a `lql *` is not internally synchronized and is supported in one
active thread at a time unless the caller serializes access externally.
Separate receivers are independent except for allocator state that a caller
deliberately shares. Production code must not use direct `malloc`, `calloc`,
`realloc`, or `free` outside the allocator implementation, and it must not
recreate `lql_alloc`/`lql_dealloc` style free wrapper functions or
`LQL_ALLOCATOR_*` macro wrappers around the allocator. Project-owned glue such
as `clql` must allocate scratch memory through private receiver allocator
helpers after `lql_new()` succeeds, without touching raw allocator accessors or
default allocator globals. Public APIs must not expose generic allocation/free
receiver methods; any future API that returns liblql-owned heap memory must
have an ownership-specific cleanup method, so downstream users do not cross
allocator boundaries or depend on allocator wrapper functions.

The public API must expose these surfaces:

- selector parse into a public selector AST object and receiver destroy;
- selector AST inspection through C-native receiver, cursor, or visitor methods;
- selector AST construction through C-native builders or node construction
  methods, so consumers can create selector trees without text parsing;
- selector AST JSON serialization and JSON parsing compatible with the Go
  selector JSON representation as structural interchange, not byte-identical
  object-member ordering;
- reusable selector plan/compiled state and receiver-based selector
  capability/trait inspection as optimization surfaces layered under or beside
  the AST;
- selector evaluation over one arbitrary JSON value;
- streaming query over arbitrary candidate streams;
- projection path parse/plan and projection execution;
- mutation parse/plan and mutation execution;
- streaming mutation over arbitrary candidate streams;
- receiver-based version and capability query methods.

The API must name behavior precisely. Do not call an API streaming unless bytes
or records flow producer-to-consumer without full-message materialization.
In particular, streaming query APIs must not copy, concatenate, retain, or
materialize complete candidate payloads behind the caller's back.
The ideal is zero allocation during steady-state query execution. Where
allocation is unavoidable, it must be explicit, bounded, and attributable to
selector state, caller-provided buffers, small parser state, selector-sized
stream scratch, or documented spooling handles rather than total input size.
Selected scalar predicates must not silently materialize full values when their
semantics can be decided incrementally. The query evaluator must not have a
selected-scalar buffer for strings or numbers, including unknown/default
selector kinds. Unknown selector kinds are invalid internal states and must not
be made to look supported by materializing selected values.

`contains` and `icontains` string predicates evaluate from lonejson scalar
chunks with a reusable suffix window sized by the selector's longest needle, so
cross-chunk matches do not require a selected-scalar buffer. Boundary checks
must iterate only suffix offsets that can cross the current chunk boundary, and
must reject impossible suffix offsets by first byte before entering full literal
comparison, using the predicate's case-folding mode before probing first-byte
metadata. `prefix` and `iprefix` predicates compare chunks directly against
selector-owned literals and retain only a bounded leading slice for short-literal
fast paths. Non-temporal `eq` and public inequality (`!=`) also compare chunks
directly against selector-owned literals plus scalar length. `in` predicates
keep selector-sized live/dead state per alternative and compare chunks directly
against selector-owned alternatives. Temporal equality fallback, temporal
inequality,
`date`, and datetime `range` predicates evaluate from bounded scalar text plus
scalar length. Numeric `range` predicates evaluate number chunks with a bounded
leading slice, a small significant-digit window, and decimal/exponent counters;
liblql must not allocate storage proportional to the selected number text.
The evaluator may derive receiver-owned execution scratch from `lql_selector`
for performance, such as a flat borrowed predicate view used by scalar
callbacks. That scratch must remain selector-derived, bounded by selector
shape, and must not become a parallel selector AST, public representation,
candidate cache, result cache, or selector-result memoization layer. Performance
work should first reduce hot-path branching, repeated selector walks, parse
passes, copies, and allocator traffic. Runtime caches that trade memory,
invalidation, or extra branches for hoped-for speed are not part of the design
unless a future decision explicitly accepts that cost. Allowed reuse is limited
to selector-owned parse-time facts and receiver-owned scratch/runtime state
whose lifetime is already tied to the public receiver and whose use does not
introduce cache lookup, eviction, coherence, or result-validity branches in the
candidate hot path.
Selectors may also retain selector-owned path metadata, such as segment offsets
and simple segment-kind tags into normalized field strings. This metadata exists
to match common absolute paths and simple wildcard segments against lonejson
decoded path segments without reparsing path text for every scalar callback. It
must be rebuilt during selector finalization, cloned as selector-owned state,
and cleaned up with the selector; recursive or escaped paths must continue to
use the general matcher.
When every predicate path is literal, scalar evaluation must skip
container-type stack tracking and should not register object/array end
callbacks. Wildcard, recursive, and other general path forms must retain that
tracking because parent container type is part of their matching semantics.
Selectors must also retain the flattened predicate pointer list during
finalization. Evaluation setup must borrow that selector-owned list and must not
walk compound selector trees per candidate to rebuild predicate scratch.
Selector finalization must derive literal lengths, max `any` literal length, and
max `in` fanout; scalar observers must use those selector-owned facts instead
of recomputing string lengths or walking the selector tree per candidate.
Selectors with `any` alternatives must also retain raw and case-folded first-byte
metadata so `contains.any` observers do not rebuild needle scan metadata per
scalar chunk.
Selector finalization must also derive predicate feature bits for contains,
prefix, exact, temporal, numeric-range, and exists families. Scalar evaluation
must use those bits to skip whole observer families when the selector cannot
match through that family.
For scalar string and number callbacks, the evaluator may prepare predicate
path-match results once at scalar begin in receiver-owned scratch and reuse that
bounded hit-index state across chunk and end observers. That scratch is bounded
by the selector predicate count and must not depend on selected scalar length.
Scalar path-match scratch should use generation or epoch marks so scalar begin
does not clear the full selector hit set for every scalar value.
The scalar path-prepare pass should also derive the active observer-family bits
and scalar scratch sizes for the current path. Scalar begin must not perform
separate full predicate-list walks for contains, prefix, exact, temporal, and
numeric-range discovery after it has already prepared path matches.
That same prepare pass should partition current-path predicates into bounded
observer-family slices for contains, prefix, exact, temporal, numeric-range,
and exists. Scalar begin, chunk, and end observers, including boolean, null,
and container observers, should iterate only their family slice instead of
rescanning the full selector predicate list, rechecking path matches for every
scalar callback, or branching away unrelated selector families. This is
receiver-owned scratch derived from the already prepared path; it is not a
selector-result cache and must not introduce invalidation or coherence checks.
Object and array begin observers should use the same prepared-path machinery
with a container-capable observer-family mask (`exists`, empty `contains`, and
empty `prefix`) so container callbacks do not run a separate full predicate
scan or branch through scalar-only observer families.
`contains.any` and `icontains.any` selectors should carry selector-owned
first-byte lookup masks so no-match scalar scans can reject most bytes in O(1)
without checking every alternative. The masks are parse-time selector metadata,
not runtime candidate allocation.
Small case-sensitive `contains.any` selectors should use the same literal
scanner as the equivalent explicit OR form when that produces fewer hot-path
branches. This is a branch-shape optimization, not selector-result caching.
Candidate hit state, stream-miss state, and `in` alternative state should also
use generation or epoch marks so candidate reset is O(1) in steady state. Full
scratch clears are allowed at query setup and on epoch wraparound; they must
not occur once per candidate.
Receiver-owned evaluator scratch should carry candidate and scalar-path epochs
across warmed queries. Query setup should not clear selector-sized hit,
stream-miss, `in`, or scalar-path match arrays unless scratch grows, scratch is
not receiver-owned, or an epoch wraps.
Steady-state decision scanning must not attempt receiver allocation after the
selector and evaluator scratch have been warmed for the same query shape. The
C allocator contract tests should freeze a receiver allocator after warmup and
rerun file, callback-source, root-array source, compound, and mixed scalar
observer-family scans; any liblql-owned allocation attempt in that warmed hot
path is a test failure.
Projection and seekable candidate mutation are held to the same warmed-receiver
standard for supported streaming paths: after parse/plan construction and
runtime warmup, repeated projection or seekable candidate mutation must not
attempt receiver allocation. This does not permit retaining candidate payloads
or full JSON documents; it only permits bounded per-receiver parser/writer
scratch reuse.

As of lonejson `v0.35.2`, the path-value visitor used by liblql still enforces
a small raw JSON number-token limit and performs bounded internal allocation for
number-token parsing. That is a dependency limitation, not permission for
liblql to copy selected numeric values. Full arbitrarily large numeric-token
support requires a lonejson public visitor mode that can stream raw number
tokens without token-size-proportional allocation and with a configurable
64-bit byte limit.

## Selector AST Public API

Selector AST access is a core SDK feature. The target is not an opaque
"compiled selector only" API. The target is a C-native public AST API that
matches the Go library's selector capabilities.

The required architecture is that `lql_selector` is the canonical selector AST,
not a facade over a separate private selector tree:

```text
text selector parser  \
selector JSON parser  -> lql_selector AST -> evaluation
public AST builders   /                   -> JSON serialization
                                            -> public traversal/cursors
                                            -> Lua selector userdata
                                            -> optional lql_selector_plan
```

This architecture explicitly rejects a hidden `lql_node`/`lql_term` AST as the
real authority underneath `lql_selector`. Private ABI-hidden fields and helper
payloads are fine, but parser, JSON, builders, evaluator, traversal,
capability inspection, and Lua must meet at `lql_selector`. A compiled
streaming/query plan may be derived from a selector for performance, but it is
execution state, not the public or internal selector AST.

The public selector API must use these C idioms:

- `lql_selector` is a receiver-compatible public and internal AST handle. It
  may remain ABI-opaque through private fields, but it must expose AST
  operations through public receiver functions or method fields.
- AST nodes are exposed as borrowed read-only cursors or views whose lifetime
  is bounded by the owning `lql_selector`. Borrowed views must not allocate
  during simple traversal.
- Node kind is reported by a public enum with all Go selector families:
  match-all/empty, `and`, `or`, `not`, `eq`, `contains`, `icontains`,
  `prefix`, `iprefix`, `range`, `date`, `in`, and `exists`.
- Child traversal exposes child count and indexed child access for `and` and
  `or`, plus one child for `not`.
- Term views expose field paths, explicit value presence, value text, `any`
  lists, `ignoreCase`, `exists` path text, range bounds, date bounds, and
  date-since macro/literal state without exposing internal temporal state.
- Range bounds expose whether they are numeric or datetime text. Datetime text
  should preserve the selector literal used to build the AST; parsed temporal
  state is implementation detail.
- Date bounds expose raw selector strings for `value`, `since`, `after`,
  `before`, `gt`, `gte`, `lt`, and `lte`, plus a macro enum for recognized
  `since` values where useful.
- AST traversal APIs must be usable from C89 and C++ consumers without private
  headers.

The public selector API must support these workflows:

- parse LQL text into an AST;
- parse a JSON selector AST payload into an AST;
- serialize an AST to the Go-compatible selector JSON representation;
- inspect node kind, children, and term payloads;
- construct an AST programmatically without passing through text syntax;
- evaluate an AST against buffered JSON and streaming candidates;
- create an optimized selector execution plan from an AST when needed;
- clone or retain AST values only through explicit ownership APIs, not hidden
  reference sharing.

The AST API must not force downstream C users to know liblql's allocator. Any
owned string or serialized payload returned by liblql must have an
ownership-specific cleanup method on the receiver or owning AST object.
Borrowed string views must document their lifetime and must not outlive the
owning selector.

The AST API must not expose Go-only implementation mechanics:

- no Go struct field layout copied into C as public mutable memory;
- no Go temporal cache type;
- no Go `url.Values` type;
- no dependence on reflection-like behavior.

The AST API does need feature-equivalent entry points for Go public behavior:

- parse from text;
- parse from structured key/value input where a C-native map or builder surface
  is provided;
- construct from public C node/builder APIs;
- serialize to and parse from JSON selector AST representation;
- inspect and traverse the AST after parsing or construction.

Until this canonical `lql_selector` architecture exists in C and is exposed
through Lua, selector parse parity, selector JSON parity, and selector
constructor parity are incomplete regardless of evaluator parity.

## Selector AST JSON And Lonejson Mapping

Selector AST JSON is part of the public selector contract. It must be parsed,
validated, and serialized through lonejson `v0.35.2` mapping, `JSON_VALUE`,
visitor, and writer surfaces. liblql must not hand-roll JSON object parsing,
escaping, raw-token decoding, or serializer formatting for selector AST JSON.

The implementation target is an internal set of lonejson-mapped transport
structs that represent the Go selector JSON shape, then convert between those
transport structs and liblql's canonical `lql_selector` AST handle:

- `Selector` object fields: `and`, `or`, `not`, `eq`, `contains`,
  `icontains`, `prefix`, `iprefix`, `range`, `date`, `in`, and `exists`.
- `and` and `or` map to recursive arrays of selector transport objects.
- `not` maps to one recursive selector transport object.
- `eq`, `contains`, `icontains`, `prefix`, and `iprefix` map to term
  transport objects.
- `range`, `date`, and `in` map to their dedicated transport objects.
- absent fields remain absent on serialization; empty selector serializes like
  the Go zero-value selector.

The transport mapping is not an excuse for another AST. It is a JSON boundary
adapter. After parse, the owned selector value is `lql_selector`; before
serialization, the source selector value is `lql_selector`.

The term transport mapping must preserve the Go omitted-value invariant:

- `field` is required.
- `value` may be absent, and absence is semantically different from an
  explicitly present empty string.
- `value` accepts JSON strings, booleans, numbers, and `null`, converting them
  to the same selector string values as Go.
- `any` accepts either a Go-compatible string form or an array; array items are
  converted to selector strings, trimmed where Go trims, and empty items are
  discarded according to Go behavior.
- `ignoreCase` accepts JSON booleans and Go-compatible string spellings where
  the Go library accepts them; invalid types or spellings fail parse.

The range transport mapping must preserve the Go `RangeBound` union:

- `field` is required.
- `gte`, `gt`, `lte`, and `lt` may each be absent.
- each bound accepts either a JSON number or a JSON string;
- numeric bounds remain numeric in the AST and in serialized JSON;
- string bounds are trimmed like Go and remain datetime text in the AST and in
  serialized JSON;
- empty string bounds are rejected.

The date transport mapping must expose and serialize raw string fields:
`field`, `value`, `since`, `after`, `before`, `gte`, `gt`, `lte`, and `lt`.
Recognized `since` macros may additionally be represented by a public C enum,
but that enum is a convenience view over the raw JSON/string contract, not a
replacement for it.

The `in` transport mapping must require `field` and preserve the `any` string
array semantics used by Go. The `exists` selector maps to the JSON string value
of `exists`.

Implementation notes:

- Prefer ordinary lonejson `lonejson_map` definitions for fixed object shapes.
  Recursive `and`, `or`, and `not` transport fields may use self-referential
  map pointers where lonejson supports object-array and nested-object maps.
- Use `lonejson_json_value` with parse visitors or path-aware visitors only for
  JSON union points that a fixed map cannot express directly, such as the
  number-or-string range bound or permissive scalar-to-string term values.
- Use lonejson writers or mapped serialization for AST JSON output. Manual
  concatenation is not acceptable, even for small objects.
- Conversion from lonejson transport structs to liblql AST nodes must allocate
  through the active `lql *` receiver allocator and must leave no lonejson-owned
  dynamic fields live after cleanup.
- AST JSON parsing may be buffer, reader, file, or path backed according to the
  public API, but the chosen behavior must be named precisely. The selector AST
  itself is naturally materialized as AST nodes; this does not relax the
  no-materialization rule for JSON candidate streams.

## Selector Scope

Selectors must converge to Go `pkt.systems/lql v0.17.1` behavior.

Required selector features:

- public parse-to-AST APIs for AND and OR text parsing;
- public AST JSON parse/serialize APIs compatible with Go selector JSON;
- public C-native AST builder APIs for all term and composition families;
- public C-native AST traversal and term inspection APIs;
- explicit terms:
  - `eq`
  - `contains`
  - `icontains`
  - `prefix`
  - `iprefix`
  - `range`
  - `date`
  - `in`
  - `exists`
- logical composition:
  - `and`
  - `or`
  - `not`
- shorthand:
  - `/field="value"`
  - `/field!=value`
  - `/field>10`
  - `/field>=10`
  - `/field<10`
  - `/field<=10`
- JSON Pointer field paths;
- array indexes;
- wildcard path semantics:
  - `*` object child values only;
  - `[]` array elements only;
  - `**` object values or array elements one level down;
  - `...` recursive descent;
  - bracket sugar such as `/items[]/sku`;
- temporal semantics:
  - date-only values;
  - RFC3339 and RFC3339Nano with uppercase `T`, uppercase `Z`, and
    `+HH:MM` / `-HH:MM` offsets;
  - naive UTC datetimes in `YYYY-MM-DDTHH:MM:SS[.fffffffff]` form;
  - date equality intersection;
  - numeric and datetime range bounds;
  - `date.since` macros: `now`, `today`, `yesterday`.
  - Go-compatible rejection of space-separated datetimes, lowercase `t`/`z`,
    malformed offsets, minute-only timestamps, and leap-second timestamps.

Unsupported selector features must be explicit in tests and benchmark output
until implemented. Silent omission is not allowed.

Selector tests must distinguish evaluator parity from AST parity. Evaluation
tests prove that a selector matches the same JSON values as Go. AST tests prove
that parsing, construction, serialization, and traversal expose the same
selector tree semantics as Go's public `Selector`, `Term`, `RangeTerm`,
`DateTerm`, and `InTerm` surfaces.

## Projection Scope

`clql -f/--field` and the C API projection surface must match Go behavior.

Projection paths are JSON Pointer paths. Multiple fields should produce the
same observable JSON structure as Go LQL. Missing projection paths should be
handled according to Go parity, including whether an output is suppressed when
no requested field is found.

Projection must use lonejson path-aware visiting/capture where possible and
must not materialize full streams unless the API is explicitly named buffered.

## Mutation Scope

Mutations must converge to Go `pkt.systems/lql v0.17.1` behavior.

Required mutation features:

- set:
  - `/state/status=running`
- numeric increment/add:
  - `/state/retries++`
  - `/state/retries=+3`
- delete:
  - `rm:/state/legacy`
- time normalization:
  - `time:/state/updated=NOW`
- brace shorthand:
  - `/state{/owner="alice",/note="hi"}`
- wildcard path behavior matching selectors where Go supports it;
- streaming file-backed mutation values:
  - `file:`
  - `textfile:`
  - `base64file:`

File-backed mutation values are opt-in and must be explicit in the API and CLI.
They must stream through lonejson source/sink behavior rather than reading
entire files into hidden buffers. Explicit `textfile:` values are text-only:
they must reject invalid UTF-8 and NUL bytes instead of silently emitting an
invalid JSON string. `file:` auto mode may classify non-text payloads as
base64-backed values.

Mutation plans may derive private parse-time traits for execution, such as
whether every path segment is a literal. Those traits exist to select a simpler
hot-path scanner for common literal set/remove/increment plans and avoid
recursive wildcard/virtual-key matching branches when they cannot apply. They
must remain derived from the canonical mutation plan, must not cache candidate
results, must not add runtime invalidation checks, and must not change wildcard,
recursive, or array-wildcard semantics.

## Streaming Query Scope

Streaming query is a core requirement.

The design target is that very large JSON inputs remain queryable on very small
machines. A 1 GB JSON input under a 128 MB process memory budget is the baseline
acceptance profile; a 1 TB JSON input on an 8 MB embedded machine is the
architectural stress model. These are not tuning goals: steady-state query
memory must be bounded by configured working buffers and parser/selector state,
not by total input size, candidate size, match count, or result set size. The
implementation must not require a complete candidate, complete stream, or
complete result set to reside in memory.

The stream API must handle:

- one top-level JSON value;
- NDJSON / repeated top-level JSON values;
- top-level arrays as streams of candidate values, including recursively
  flattened nested top-level array candidates where the public API can do so
  without hidden full-value materialization;
- large candidates with bounded memory;
- decision-only mode;
- plus-value mode with callback-scoped payload access;
- matched-only callbacks;
- seekable/rewindable sources where matched payload access is reconstructed
  from candidate offsets and byte sizes without capture;
- caller-managed or lonejson-managed payload sinks when supported, without
  liblql retaining complete candidate copies;
- non-seekable sources where plus-value output uses caller sinks or bounded
  spooled handles rather than memory capture;
- stop controls:
  - max matches;
  - max candidates;
  - max bytes read;
  - callback-requested graceful stop.

Candidate payload access must distinguish:

- no payload captured;
- seekable source range: candidate offset plus byte size, read only after a
  match decision;
- callback-scoped streaming/sink payload access;
- callback-scoped spooled payload handles;
- caller-managed payload sinks.

Streaming query must not use in-memory compact JSON capture as an
implementation shortcut. Any future API that deliberately materializes complete
candidate payloads must be named as buffered/materialized behavior, must be
opt-in, and must not be used to satisfy the streaming query requirement.
For very large matching values, plus-value behavior must expose a streaming
seekable range, sink/source, or spooled handle; it must not construct one
contiguous in-memory JSON value merely to return it to the caller. If the input
source is seekable or rewindable, liblql must prefer offset/size based reread
over candidate capture. Capture is only justified when the source cannot be
revisited or when the caller explicitly selects a capture mode.

As of lonejson `v0.35.2`, the installed public header confirms that
`lonejson_candidate_info` exposes candidate index, stream offset, byte size, and
payload size as `lonejson_uint64` range values, and exposes public candidate
streaming without payload capture. liblql should therefore treat lonejson's
candidate streaming and 64-bit range metadata as sufficient for decision-only
candidate streaming and seekable-source payload reconstruction. The
implemented liblql source policy is therefore a library/API concern rather than
a JSON parser workaround:

- public `FILE *` APIs are the seekable/rewindable source surface;
- callback-source APIs are the non-seekable source surface;
- matched seekable candidates expose callback-scoped seekable range payloads;
- seekable decision-only and seekable plus-value paths use lonejson
  `CAPTURE_NONE`;
- callback-source decision streams discard ordinary candidate payload bytes as
  they are parsed and use callback-scoped spooled replay only for nested
  top-level array candidates because non-seekable sources cannot be rewound by
  offset;
- callback-source decision streams may inspect one bounded prefix chunk to route
  ordinary non-array streams through lonejson `CAPTURE_NONE`; if the prefix is
  empty, all whitespace, or starts with `[`, they must retain sink capture so
  root-array recursion remains correct;
- non-seekable plus-value paths use callback-scoped spooled handles and caller
  sinks rather than contiguous candidate materialization.

The public liblql v0 callback-source framing contract covers one stream of
top-level JSON values and root-array item streams as exposed by lonejson
`v0.35.2`. It does not claim the Go implementation's narrower mixed framing
case where a non-seekable source starts with a top-level array and then
continues with more top-level values. That shape is an accepted v0 non-parity
case documented in `docs/liblql-dependency-gaps.md`, not current liblql
implementation work. liblql must not materialize the root array or the whole
source to emulate it.

Seekable range APIs use 64-bit liblql offsets and sizes. When a platform
`FILE *` seek cannot represent a 64-bit range offset, liblql must fail the
operation rather than truncating or wrapping the requested offset.

## CLI Scope

`clql` should mirror the Go `lql` CLI where supported:

- selector arguments;
- `--or` / `-O`;
- `--field` / `-f`;
- `--mutate` / `-m`;
- `--inline` / `-i`;
- `--write` / `-w`;
- `--compact` / `-c`;
- `--matches-only` / `-M`;
- `--enable-file-mutations` / `-F`;
- `--help` / `-h`;
- `--version` / `-v`.

`--theme` / `-t` is not part of the `clql` CLI. The Go CLI uses theme options
only for `prettyx` colorized output, which this port intentionally does not
ship. `clql` must not accept no-op compatibility flags for unimplemented
features.

The CLI must produce actionable errors with stable wording where practical.
`clql -h` and `clql --help` are not required to copy the Go `lql` help text.
They must present a C-native help surface with a concise summary, usage forms,
grouped options, and examples for selection, projection, and mutation.

## Lua Scope

The Lua implementation is part of this repository's parity story.

Lua should expose the same high-value behavior as the C library:

- selector parse/evaluate and selector AST inspection/construction;
- selector AST JSON round-trips compatible with the Go selector JSON shape;
- streaming query;
- projection;
- mutation;
- parity benchmark entry points.

The Lua implementation should use public liblql/lonejson surfaces rather than
duplicating LQL behavior independently unless an explicit Lua facade layer is
needed for DX.

Lua support targets Lua 5.5 only. The Lua facade must not shell out to `clql`;
it must load a direct C module that depends on the public liblql SDK surface.
The module must not use private liblql headers, private lonejson APIs, or
symbol interposition tricks, and it must be safe to load in a process that
already links liblql and other Lua bindings.

The Lua facade must expose a real liblql client object. `lql.new()` owns a
public `lql *` receiver created through `lql_new()`, and method calls dispatch
through that receiver. Lua must not emulate the client with a table of
module-level wrappers, and it must not depend on `clql`.

Lua selector values must be userdata backed by the public liblql selector AST
API. The Lua facade must not carry an independent selector parser, an
independent mutable AST table model, or a hidden Go-shaped reimplementation.
A Lua user must be able to parse selector text, inspect the AST through userdata
methods, construct a selector AST through C-backed constructors or builders,
serialize it to the Go-compatible selector JSON form, parse it back from that
JSON form, and use it for query/mutation workflows through the same liblql
receiver instance. Lua table helpers may exist for ergonomic construction or
inspection, but they are conversion facades over C selector userdata, not the
authoritative AST representation.

## Verification Requirements

Verification is the primary quality gate.

The verification strategy has three distinct layers:

- C-native product tests define what `liblql` and `clql` promise to downstream
  C, CLI, and Lua users. These tests assert public API ownership, callback
  lifetimes, out-parameter state, partial I/O, cleanup, diagnostics, selector
  AST traversal/construction, bounded-memory semantics, and observable
  selector/projection/mutation behavior.
- Go oracle tests compare claimed LQL language and transformation behavior
  against `pkt.systems/lql v0.17.1` while the port is converging. They are
  convergence checks, not C unit tests.
- Benchmarks verify behavioral counters across implementations and, for mature
  public `liblql` paths, enforce C-native performance and memory expectations.
  Go timing is context, not the desired C performance level.

Required gates:

- C SDK contract tests for every public liblql behavior, with observable
  assertions for the C API's ownership, error, callback, streaming, selector
  AST, bounded-memory, and result semantics;
- public header standalone compile tests;
- C89 consumer tests;
- CMake install-tree consumer tests;
- pkg-config consumer tests;
- `clql` smoke tests;
- Go library parity tests through `parity/` while the port is converging; these
  are oracle/reference checks for selector language, selector AST semantics,
  and transformation behavior, not substitutes for C API contract tests and not
  a source to mechanically copy into C unit cases;
- Go CLI parity tests for `clql` versus the Go `lql` binary behavior where the
  C CLI claims compatibility;
- Lua parity tests when Lua facade exists;
- sanitizer tests;
- package verification and privacy/relocatability gates;
- source archive smoke tests;
- Go/C/Lua behavioral benchmark records plus C-native performance and memory
  gates for mature public `liblql` paths.

Fast test command contract:

- `make test` runs the fast C/API test surface and excludes parity-labeled
  transitional Go checks;
- native `lql.unit` coverage must stay effectively immediate and subsecond on
  ordinary developer hardware; if a C unit test needs corpus-scale input,
  sleeps, retries, benchmark timing, release packaging, Go execution, Lua
  execution, or large-fixture generation, it belongs in a narrower explicit
  parity, benchmark, fuzz, package, or release gate instead;
- C-only unit tests may assert streaming and bounded-memory behavior with small
  deterministic fixtures and instrumentation, but must not prove those
  properties by spending noticeable wall-clock time or materializing large
  documents;
- `make parity-test` runs the Go-backed CLI and SDK parity suites explicitly;
- `make fuzz-smoke` runs bounded public API seed coverage over parser,
  projection, compaction, mutation, and streaming query entry points;
- `make test-all` includes the fast C/API tests, parity tests, fuzz smoke,
  sanitizer checks, and Lua checks.

Parity benchmark spec:

- `docs/liblql-parity-benchmark-spec.md`

Selector AST implementation spec:

- `docs/liblql-selector-ast-spec.md`

Tests should assert observable behavior, not implementation details.

Parity has two separate meanings in this repository:

- SDK parity: public `liblql` behavior must converge to the Go `pkt.systems/lql`
  library behavior for the LQL language, public library capabilities, and data
  transformations. Public Go capabilities such as selector AST parsing,
  construction, traversal, and JSON representation require idiomatic C and Lua
  equivalents. They do not require Go struct layout or Go implementation
  mechanics. The Go-backed parity suite is the oracle for convergence and
  regression detection while the port is incomplete. It must not be treated as
  C unit coverage, and C unit tests must not be generated by mechanically
  copying Go parity rows. C tests own the C-native public contract: handle
  lifetime, ownership, AST traversal ownership, out-parameter state, callback
  behavior, partial I/O, explicit buffered versus streaming APIs, failure
  diagnostics, cleanup after errors, bounded memory, and other behavior that
  downstream C consumers rely on.
- CLI parity: `clql` must converge to the Go `lql` command's observable CLI
  behavior for supported flags and workflows, excluding prettyx colorized JSON.
  CLI parity tests are command-level oracle checks. They do not prove SDK
  contract coverage unless the same behavior is independently meaningful and
  tested through public liblql APIs.

Both surfaces need executable coverage manifests, but the manifests serve
different purposes. Go-backed manifests classify oracle coverage. C manifests
classify product contract coverage. A claimed public SDK behavior needs
C-native tests for its public C contract; it does not need a C clone of every
Go parity example. Manifest gates must reject duplicate test-function entries
and duplicate `surface/requirement` keys so coverage cannot be overstated by
double-counting either a test or a claimed requirement.

The full parity audit is tracked by `parity/oracle_inventory.tsv` and enforced
by `TestOracleInventory` in the parity module. That inventory is file-level
rather than workstation-path based: it records every test, benchmark, and
example-bearing file in the pinned `pkt.systems/lql v0.17.1` module, the
current coverage status, evidence, and the next action. The gate fails when the
pinned Go oracle changes without inventory updates. `covered` rows can support
a final parity claim only together with the cited C/CLI/SDK/Lua tests. If
`partial` or `gap` rows exist, they are explicit remaining work.
`not-applicable` rows must state why the Go behavior is genuinely
implementation-specific rather than a public feature that needs an idiomatic
C/Lua representation. The current v0 inventory has no `partial` or `gap` rows.

## C-Native Strategy

The port must be a C implementation, not a Go implementation transliterated
into C. Design decisions should favor explicit ownership, streaming data flow,
bounded memory, predictable error surfaces, and use of lonejson's native
streaming/range APIs. Full-document materialization, per-candidate scalar-list
materialization, hidden buffering, and ad hoc JSON handling are implementation
smells unless the public API is explicitly named as buffered and caller-owned.

Development should proceed in larger outcome slices:

- complete a behavior surface, such as selector AST, selector evaluation,
  streaming payloads, projection, mutation, CLI workflow, Lua facade, or release
  packaging;
- implement the C-native behavior and its public contract tests together;
- run Go parity as an oracle to catch semantic drift from the reference;
- add or update benchmarks when performance or memory behavior is part of the
  surface;
- commit the completed surface slice.

Idiomatic C may map Go public structs to receiver shells, borrowed cursors,
builders, visitors, and explicit cleanup methods. It may not replace a public
Go AST feature with an opaque evaluator-only handle and call that parity.

Do not spend implementation cycles mining Go parity cases solely to duplicate
them in C tests. When Go parity exposes a divergence, fix the C behavior, then
add C tests only for the C API invariant or product behavior that should have
prevented the bug. Exhaustive C coverage means the C product contract is
covered surface by surface; it does not mean every Go oracle row has a
mechanical C twin.

## Performance Strategy

The Go implementation is a semantic reference, not a performance target. A C
port that merely matches Go throughput is suspect unless the benchmark is
dominated by external I/O, process startup, or another documented non-library
cost. `liblql` should normally be substantially faster and more memory-stable
than Go for library-level streaming selector, projection, mutation, and payload
access paths.

Performance gates must therefore validate two things separately:

- behavioral parity: candidates, matches, payload counts, payload bytes, and
  parse/error outcomes agree with the oracle for supported behavior;
- C-native performance: steady-state memory remains independent of total input
  size, candidate size, match count, and result size, and C library paths are
  bounded by native C thresholds rather than Go throughput.

Initial benchmark gates include behavioral checks, bounded RSS, and a loose
native C steady-state ns/byte ceiling for supported C records. Once a surface is
mature enough for tighter guarantees, the benchmark should move to a documented
C-native threshold, baseline, or speedup floor for that specific public path.
CLI-mediated and Lua facade timing may remain report-only where process startup
or facade overhead dominates the measurement, but claimed public liblql and Lua
memory behavior must be gated rather than treated as report-only.

## Packaging Requirements

Packaging follows the pkt.systems lifecycle.

Binary SDK archives must be relocatable and must not contain:

- source checkout paths;
- build paths;
- dependency cache paths;
- `$HOME`;
- absolute local `file://` URLs;
- sanitizer runtime or debug metadata;
- non-relocatable ELF RPATH/RUNPATH;
- non-relocatable Darwin Mach-O install names, dependency paths, or rpaths.

`package-verify` must expand checksum-listed artifacts and nested archives
before scanning. For Darwin archives, package verification must inspect
extracted final binaries and dylibs with target-correct `otool`; absence of
that inspection tool is a release-blocking verification failure for a packaged
Darwin artifact. Darwin final artifacts are not routinely post-processed with
`strip` or `install_name_tool`; the release path must produce correct Mach-O
loader metadata at CMake link/install time and then verify it.

`dist/` is generated output. Checksums/manifests define upload artifacts.

## Iteration Plan

The port should progress in falsifiable slices:

1. Lifecycle foundation
   - CMake/Make/scripts/presets;
   - lonejson dependency acquisition;
   - initial package surfaces.

2. Selector AST foundation
   - canonical `lql_selector` AST handle, cursors, visitors, and builders;
   - remove private selector-tree authority such as `lql_node`, `lql_term`,
     and `LQL_NODE_*` as the implementation boundary;
   - parse LQL text into `lql_selector` for AND and OR entry points;
   - parse and serialize Go-compatible selector AST JSON through lonejson
     mappings, JSON_VALUE adapters, and writers;
   - C-native AST traversal, construction, ownership, and JSON tests;
   - Lua selector userdata facade backed by public liblql APIs, with optional
     table conversion helpers only as C-backed convenience;
   - Go-vs-C/Lua AST parity checks for text parse, constructors, JSON
     round-trips, and omitted-value semantics.

3. lonejson upgrade integration
   - consume candidate stream with 64-bit ranges;
   - consume path-aware visitor and JSON Pointer helpers;
   - remove liblql-owned path reconstruction and scalar-list materialization
     where possible.

4. Selector evaluation core
   - evaluate scalar selectors from the public AST;
   - derive optimized selector plans from the AST where useful;
   - Go parity tests for supported evaluation subset.

5. Streaming query foundation
   - decision-only candidate stream over `FILE *`;
   - candidate index, offset, and byte size callbacks;
   - no payload capture in decision-only mode.

6. Full selector parity
   - wildcards;
   - temporal selectors;
   - `in`;
   - selector AST/evaluator/plan agreement.

7. Streaming query parity
   - seekable source range payload handles;
   - non-seekable caller sink/spool payload handles;
   - stop controls.

8. Projection parity
   - field selection;
   - CLI `-f`.

9. Mutation parity
   - parse/plan;
   - multi-path rewrite;
   - file-backed mutation values;
   - inline write behavior.

10. Lua parity
   - facade and tests;
   - parity benchmarks.

11. Packaging completion
   - liblql SDK archives;
   - clql archives;
   - release matrix verification.

Each slice must add or update C-native product tests before claiming support.
Go oracle coverage must also be updated when the slice changes claimed LQL
language, transformation, or CLI compatibility behavior. A slice is not
complete merely because Go parity passes.

## Current Repository Status

Current implementation status:

- lifecycle scaffold exists;
- lonejson `v0.35.2` binary archive acquisition from GitHub release assets
  exists;
- the installed public C API now exposes selector AST traversal, construction,
  and Go-compatible selector JSON parse/serialize through receiver methods.
  The C internals now use `lql_selector` as the recursive selector AST; the
  previous private `lql_node`/`lql_term` authority has been removed.
  The Lua facade now exposes selector userdata backed by the public C selector
  API for parse, AST JSON import/export, traversal, builders, and query reuse.
  Selector-library parity is still not complete until the remaining oracle
  inventory audit is closed;
- `make test` includes repository-boundary checks that fail if committed
  repository files reference the adjacent Go source checkout through
  `../lql`-style paths or workstation-local checkout paths; parity remains
  routed through the pinned `parity/` Go module;
- fast CTest compiles installed public headers as both C90 and C++98 with
  warnings treated as errors, proving the declared C API remains usable from C
  and C++ consumers;
- the public C API exposes an instantiatable receiver shell through `lql_new()`
  and method-pointer dispatch; each receiver owns private implementation state
  carrying the internal allocator receiver used for receiver cleanup, and the
  C-native receiver test fails if a constructed receiver does not populate that
  private state; selector/query/projection/mutation operations are implemented
  by receiver-compatible private functions and are not exported as
  free-function wrappers; every installed receiver data or method field has an
  ownership/error-behavior comment in `include/lql/lql.h`;
- `make test` includes `lql.public-api-style`, which fails if removed
  selector/query/payload/projection/compact/mutation free-operation prototypes
  reappear in the installed header or, when a shared library is built, as
  exported dynamic symbols; for shared builds it also allowlists the complete
  exported `lql_*` symbol set to construction and diagnostics only, including
  payload write/projection operations that
  must remain receiver methods and must not gain standalone compatibility
  wrappers after the receiver refactor; it also scans project-owned source trees for
  exact-name macro or static wrapper shims that recreate those removed
  operation functions and for static receiver-operation shims that should have
  been folded into the receiver-compatible implementation functions; receiver
  cleanup fields named `*_free`, project-owned cleanup helpers named
  `free_*`/`*_free`, and public allocator wrapper prototypes are also rejected
  so cleanup stays on the `*_destroy` receiver surface and the allocator
  boundary stays internal; the same style gate rejects undocumented public
  receiver fields so the installed SDK surface remains self-describing; the
  style fixture suite includes negative cases for public cleanup wrappers,
  public allocator-free wrappers, receiver `*_free` fields, static cleanup
  wrappers, direct runtime allocation, old allocator wrapper calls, and
  `LQL_ALLOCATOR_*` macro wrapper calls;
- `make test` includes `lql.script-modes` and fixture coverage that fails when
  repository-owned shell entrypoints under `scripts/` are not executable, so
  lifecycle and API-style gates remain directly runnable developer surfaces
  instead of relying only on `sh script` invocation from CTest;
- Go-backed SDK parity remains an oracle harness, not an alternate C SDK
  surface: C helpers in `parity/sdk_liblql.go` that perform liblql operations
  must receive `lql *ctx` as their first argument and dispatch through
  `ctx->method(ctx, ...)`; the only free helper exceptions are local utilities
  that do not operate on a receiver, such as temporary file readback and test
  input preparation. `lql.public-api-style-fixtures` includes a negative
  parity helper that omits the receiver argument and proves the style gate fails
  closed;
- project-owned allocations have an internal per-receiver liblql allocator
  surface for receiver-owned glue buffers, but no public generic
  allocation/free receiver methods;
  the old `lql_alloc`/`lql_calloc`/`lql_realloc`/`lql_dealloc`/`lql_strdup`
  wrapper layer and the private `LQL_ALLOCATOR_*` macro wrapper layer have been
  removed, and direct C runtime allocation calls are limited to the allocator
  implementation; `make test` enforces this by failing on old allocator wrapper
  calls, allocator macro wrapper calls, public receiver memory wrappers, raw
  allocator accessor/default allocator use from `clql`, or direct
  `malloc`/`calloc`/`realloc`/`free`/`strdup` calls in project-owned C
  sources outside `src/lql_allocator.c`;
- lonejson parser/serializer runtimes created by liblql core are constructed
  through a private receiver allocator bridge, not `lonejson_new(NULL, ...)`.
  This keeps lonejson-owned transient parse, visitor, writer, stream, spool, and
  output buffers under the active `lql *` allocator policy; the public API style
  fixture suite rejects direct default-allocator lonejson runtime construction
  in core sources, and `lql.handle-allocator` asserts that receiver-backed JSON
  evaluation performs its transient runtime allocations through the counting
  receiver allocator and returns to the prior outstanding allocation count;
- selector, projection, and mutation plan ownership is receiver-consistent:
  receiver parse methods pass the receiver allocator into handle parsers, but
  produced `lql_selector`, `lql_projection`, and `lql_mutation_plan` handles do
  not store allocator pointers or fallback cleanup domains; persistent parse
  state and runtime scratch buffers are cleaned up through the active receiver
  allocator supplied by the calling method. The public API style gate rejects
  direct `lql_allocator_default()` calls in `src/lql_selector.c`,
  `src/lql_project.c`, `src/lql_eval.c`, and `src/lql_mutation.c`, and also
  rejects allocator fields inside receiver-owned child handle structs, so
  library core code cannot bypass receiver allocator ownership by making child
  handles self-owning. Projection and mutation handle cleanup helpers are also
  style-gated to take the owning `lql *self` rather than an allocator-first
  parameter, so nested handle cleanup cannot accidentally detach from receiver
  ownership while still using the same per-receiver allocator implementation.
  Projection parser boundary helpers are likewise style-gated to derive
  allocation from `lql *self`, and mutation expression parser helpers are
  style-gated to receive a `mutation_parse_context` built by the receiver
  method, so method-level parser control flow does not preserve
  allocator-passed mini-entry-points alongside the receiver API. Runtime
  scratch helpers for projection, selector eval, and mutation streaming are
  also style-gated to receive the owning runtime context rather than a detached
  `lql_allocator *`, so streaming callbacks keep allocator use attached to the
  receiver-owned execution state.
  `lql.handle-allocator` constructs real receivers with a
  counting internal allocator rather than fabricating private `lql` state,
  exercises successful and failed selector, projection, and mutation parses
  plus selector eval, projection runtime, and mutation runtime through receiver
  method dispatch, proves projection of a large unselected scalar does not
  allocate in proportion to that scalar's byte length, and proves handle
  allocations, runtime scratch buffers, and receiver-owned allocations return
  to zero outstanding allocations on cleanup;
- the public API style gate rejects selector/query/payload/projection/compact/
  mutation `_impl`, `lql_eval_selector`, `lql_eval_query_*`,
  `lql_project_spooled`, `lql_mutate_spooled_paths`, and
  `lql_parse_selector_internal()` operation calls from tests, examples, and
  benchmarks, so executable examples and C-side product tests exercise liblql
  through the receiver surface instead of preserving private operation
  bypasses; core implementation files are also style-gated against restoring
  shared hidden `lql_*` operation entry points for receiver-owned behavior;
- the SDK contract manifest gate parses the installed receiver method table and
  fails if any public receiver method lacks C-side method-call coverage in
  `tests/test_lql.c`, so method-table growth cannot silently outrun native SDK
  tests; `lql.sdk-manifest-fixtures` proves the gate fails when a receiver
  method lacks C-side coverage or when a C SDK unit exists outside the
  manifest;
- core code is style-gated against calling private receiver implementation
  functions with a `NULL` receiver; parse-failure cleanup paths must use the
  explicit owning allocator or handle cleanup surface instead of routing
  through operation-shaped null-receiver calls;
- query evaluator receiver methods carry the active `lql *` receiver through
  file-local execution helpers, so selectorless/match-all query scratch state
  and nested projected mutation execution use the same receiver context instead
  of a null-receiver default allocator fallback; projection and mutation of
  callback-scoped spooled payloads re-enter `project_source` and
  `mutate_source_paths` through bounded source callbacks rather than sharing
  separate `lql_project_spooled` or `lql_mutate_spooled_paths` operation
  entry points;
- projection, compact, and mutation runtime helpers also carry receiver context
  across private helper boundaries; core source files are style-gated against
  `lql_allocator_from_receiver(NULL)` so library behavior cannot silently fall
  back to default allocator ownership when a receiver is available;
- selector node cleanup requires its caller to provide the owning receiver, and
  the receiver allocator accessor must fail closed rather than falling back to
  the default allocator for invalid receivers; core cleanup paths are
  style-gated against reintroducing `allocator = lql_allocator_default()`
  fallback branches, so cleanup ownership remains explicit instead of silently
  crossing allocator domains;
- receiver methods are installed through module-owned method installers and
  implemented as file-local method bodies rather than private `*_impl`
  operation symbols; `lql.public-api-style-fixtures` includes negative cases
  proving both private `_impl` calls and private `_impl` method definitions fail
  closed;
- version and capability queries are receiver-only for public consumers:
  `clql`, Lua, examples, header smoke consumers, and package smoke consumers
  must call `ctx->version(ctx)` and `ctx->capabilities_get(ctx, ...)` so the
  documented and exercised product surface stays receiver-first after
  construction;
- selector capability and execution-trait inspection is receiver-only through
  `ctx->selector_capabilities_get(ctx, selector, ...)` and
  `ctx->selector_execution_traits_get(ctx, selector, ...)`; the methods walk the
  parsed selector handle without allocating, copying selector data, or
  materializing JSON, and C contract tests cover feature-family flags,
  wildcard/recursive path flags, empty/match-all simplification, null
  out-parameter tolerance, and early-non-match trait behavior;
- C SDK selector tests include quoted selector literals as native product
  behavior: comma-containing equality values, space-containing contains values,
  quoted JSON Pointer text, and multi-clause quoted values must parse and match
  without relying on Go parity as the only guard;
- decision-only candidate streaming over `FILE *` uses lonejson candidate
  streams with `CAPTURE_NONE` and 64-bit candidate ranges;
- seekable `FILE *` range rereads reject offsets that cannot round-trip through
  the platform `off_t`, so large-range failures are explicit instead of
  truncated;
- decision-only `FILE *` query streams expose stop controls for match count,
  candidate count, bytes read, and callback-requested graceful stop;
- decision-only callback-source query streams expose the same decision and
  stop-control behavior over caller-provided read callbacks with no candidate
  payload capture;
- callback-source matched-candidate query streams expose callback-scoped
  `LQL_PAYLOAD_SPOOLED` payload handles and receiver payload writer support for
  non-seekable plus-value access without retaining payloads after the match
  callback returns;
- C SDK source-reader contract tests reject callbacks that report
  `bytes_read > capacity` across query decision streams, spooled match streams,
  projection, compaction, direct source mutation, source candidate mutation, and
  projected source candidate mutation, proving liblql validates caller callback
  behavior instead of trusting invalid producer lengths;
- matched-candidate `FILE *` query streams expose callback-scoped
  `LQL_PAYLOAD_SEEKABLE_RANGE` payload handles, with
  `ctx->payload_write_json()` and `ctx->payload_write_json_sink()` preserving the
  parser source position while rereading the matched candidate range; this path
  still uses candidate `CAPTURE_NONE` and does not retain candidate JSON;
- seekable and spooled payload handles can be written to caller-managed sink
  callbacks through `ctx->payload_write_json_sink()` without requiring a
  `FILE *`;
- seekable and spooled payload handles can be projected during callback scope
  through `ctx->payload_project_json()` without materializing the complete
  candidate in liblql;
- selection-mode `clql -M/--matches-only` matches Go CLI behavior by writing
  matched JSON candidates and returning success even when no candidates match;
  mutation-mode `-M` remains the Go-compatible output filter for matched
  candidates only;
- default `clql selector < data.json` output streams non-seekable stdin
  through lonejson candidate parsing with callback-scoped spooled candidate
  payloads, so it no longer reads the complete stdin stream into memory before
  deciding matches;
- `clql -c/--compact selector < data.json` compacts matched non-seekable stdin
  candidates by streaming callback-scoped spooled payloads back through
  lonejson rather than materializing complete candidates in liblql;
- `clql -f/--field selector < data.json` projects matched non-seekable stdin
  candidates by streaming callback-scoped spooled payloads through the public
  projection visitor path, without retaining complete candidates in liblql;
- `clql` accepts an empty selector for match-all file selection, including the
  Go-compatible shorthand where a single existing file path is the input rather
  than selector text;
- `clql` accepts multiple selector arguments, combines them with default AND
  semantics, and honors `--or` / `-O` for OR composition before the final file
  or `-` input argument;
- `clql selector data.json` uses seekable candidate offset/size ranges to
  reread and write matched payloads without full-input materialization;
- `clql -c/--compact selector data.json` compacts matched seekable file ranges
  through lonejson without candidate materialization;
- `clql -f/--field selector data.json` supports root, nested object-field,
  array-index, and escaped JSON Pointer projection on seekable file inputs
  using the public projection API, lonejson path visiting, and writer output;
- the initial C projection API exposes receiver methods
  `ctx->projection_parse()` and `ctx->project_file_range()` for object and
  array-index paths over seekable file ranges, `ctx->project_source()` for
  caller-provided read callbacks, plus `ctx->project_json()` for explicitly
  caller-buffered JSON values;
- the initial C compact API exposes `ctx->compact_file_range()` for streaming
  seekable ranges, `ctx->compact_source()` for caller-provided read callbacks,
  and `ctx->compact_json()` for explicitly buffered JSON values;
- the public C API installs generated `lql/version.h` version macros and
  exposes runtime version/capability queries through the receiver, with package
  and source-archive verification proving the generated header and capability
  query build from installed and extracted trees;
- the initial C mutation API exposes receiver methods
  `ctx->mutation_plan_parse()`, `ctx->mutation_plan_parse_with_options()`,
  `ctx->mutation_plan_count()`, and `ctx->mutation_plan_destroy()` for CLI-style
  mutation parse/plan validation;
  file-backed mutation values remain disabled by default and require explicit
  parse options;
- mutation execution APIs expose
  `ctx->mutate_file_range_root_fields()` for bounded source-backed rewrites of
  root object fields and `ctx->mutate_file_range_paths()` for bounded
  source-backed rewrites of concrete object/member paths with optional concrete
  array indexes and existing-position `*` object-child or `[]` array-element
  wildcards over seekable file ranges; existing object-member and array-element
  mutation positions also support `**` one-child and `...` recursive path
  segments; Go-compatible stream mutation treats concrete numeric descendant
  paths under an existing array as object-key creation, so `/items/0=x`
  rewrites `items` as an object member named `"0"` rather than preserving the
  source array, while explicit `[]` wildcard mutation remains array traversal;
  supported set values include `time:` normalization to UTC RFC3339Nano strings
  and `file:/textfile:/base64file:` source-backed file values; explicit
  `textfile:` execution rejects invalid UTF-8 and NUL bytes while `file:` auto
  mode can classify those payloads as base64-backed values; public C
  execution is currently available for seekable file ranges,
  seekable candidate streams through `ctx->mutate_file_range_candidates()` and
  `ctx->mutate_file_range_candidates_with_options()`,
  projection-before-mutation seekable candidate streams through
  `ctx->mutate_file_range_projected_candidates()` and
  `ctx->mutate_file_range_projected_candidates_with_options()`,
  caller-provided read callbacks through `ctx->mutate_source_paths()`,
  callback-source candidate streams through
  `ctx->mutate_source_candidates()` and
  `ctx->mutate_source_candidates_with_options()`,
  projection-before-mutation callback-source candidate streams through
  `ctx->mutate_source_projected_candidates()` and
  `ctx->mutate_source_projected_candidates_with_options()`, and
  explicitly caller-buffered JSON values through `ctx->mutate_json()`;
- `clql -m/--mutate` emits all seekable file candidates in mutation mode,
  applies supported concrete-path, existing-position wildcard, and
  existing-position recursive mutations to matched candidates;
- public SDK seekable and callback-source candidate-stream mutation exposes
  the selector-plus-plan path used by `clql`: top-level arrays are expanded as
  candidate streams, nested top-level array candidates are recursively
  flattened where the stream surface supports candidate payload access, matched
  candidates are mutated, unmatched candidates are preserved unless
  `matches_only` is set, match-all candidate-stream mutation mutates every
  top-level array candidate, option-aware variants enforce match, candidate, and
  byte stop limits, and result counters report candidates and matches;
- brace shorthand mutation execution is covered by C SDK contract tests and
  Go-backed CLI parity tests for nested set, increment, delete, and path
  expansion behavior;
- escaped JSON Pointer mutation paths are covered by C SDK contract tests and
  Go-backed CLI parity tests for set, delete, and nested path expansion
  behavior;
- quoted mutation values are covered by C SDK contract tests and Go-backed CLI
  parity tests; JSON-looking quoted values follow Go typing behavior, so quoted
  numeric, boolean, and null literals are emitted as JSON values rather than
  strings while ordinary escaped text remains a JSON string;
- mutation parser and execution coverage includes Go-compatible decrement and
  signed-delta increment spellings plus `rm:`, `remove:`, `delete:`, and `del:`
  delete aliases;
- `clql -m/--mutate selector < data.json` applies the same supported streaming
  mutation subset to matched non-seekable stdin candidates through
  callback-scoped spooled payloads, preserves unmatched candidates by default,
  and honors `-M/--matches-only` without materializing the full input stream;
- `clql` execution paths now dispatch through the public `lql *` receiver
  methods instead of calling private `lql_eval_selector`, `lql_eval_query_*`,
  `lql_project_spooled`, `lql_mutate_spooled_paths`, or private `_impl`
  receiver entry points directly; `lql.public-api-style` rejects those private
  execution shortcuts in `src/clql.c` so CLI behavior remains aligned with the
  library surface rather than an internal-only path;
- `clql` process-glue allocations for parsed argv lists, joined selector text,
  and inline temp path ownership use private receiver allocator helpers after
  `lql_new()` succeeds; the style gate rejects raw
  `lql_allocator_from_receiver()` and direct `lql_allocator_default()` use in
  `src/clql.c`, and null-receiver allocator fallback in project runtime code,
  so argv-derived buffers cannot be allocated before receiver construction or
  freed through a different allocator domain while still keeping generic
  allocation/free wrappers out of the public SDK;
- `clql -m/--mutate -f/--field` follows Go CLI order for seekable file input
  and non-seekable stdin: project each output candidate first, then mutate the
  projected value only for matched candidates; this path uses callback-scoped
  temp-file spill for the projected candidate and does not retain projected
  JSON in memory;
- `clql -m/--mutate` accepts a single file argument with no selector as
  match-all mutation input and rejects multiple file inputs explicitly;
- match-all `clql -m/--mutate` over mixed candidate streams preserves
  non-object candidates unchanged while applying field mutations to object
  candidates;
- `clql -F/--enable-file-mutations` opts into parsing file-backed mutation
  values and supports `file:`, `textfile:`, and `base64file:` streaming
  execution over seekable file input and supported spooled stdin mutation,
  rejects invalid UTF-8 and NUL bytes for explicit `textfile:` payloads with
  actionable execution diagnostics, and includes `~/` home-directory expansion
  for file-backed value paths;
- `clql -m -M/--matches-only` emits only matched seekable file candidates after
  applying supported concrete-path mutations;
- `clql -m -i/--inline` and `clql -m -w/--write` rewrite a single seekable
  input file through a sibling temp file and rename only after successful
  mutation; stdin and non-mutation inline use are rejected;
- current selector subset evaluation uses lonejson path-aware visitor callbacks
  and marks selector term hits as values stream through, rather than building a
  per-candidate scalar document list; scalar chunk buffering is gated by
  selector-relevant paths, unrelated large string and number values are not
  accumulated merely because they appear in a candidate, selected
  `contains`/`icontains` string predicates stream through bounded suffix state,
  selected `prefix`/`iprefix` predicates retain only a bounded leading slice,
  and selected non-temporal `eq`, public inequality (`!=`), plus `in`
  predicates retain only a bounded leading slice plus scalar length, while
  selected temporal equality fallback, temporal inequality, `date`, and
  datetime `range` predicates retain only bounded scalar text plus scalar length
  instead of retaining the full selected scalar;
- first C selector parse/evaluate subset exists, including
  `contains.any`, `icontains.any`, `in.any`, wildcard selector paths,
  single-quoted selector values containing spaces or commas, quoted JSON
  Pointer text for `exists`, bracket-sugar array paths, string-term
  `ignoreCase`/`ic` flags, and
  multi-bound numeric ranges, with omitted string-term values treated as path
  assertions that include JSON `null`, while explicit `exists`, `eq`, `in`, and
  valued string terms do not treat JSON `null` as an empty string or non-null
  value; temporal `date` terms, datetime `range` bounds, and relative
  `date.since` macros are implemented; nested indexed `and.N` / `or.N`
  logical wrapper groups, including `and.or.N` and `or.and.N` wrapper chains,
  are parsed and evaluated recursively; concrete numeric path segments are
  covered as both object keys and array indexes through receiver selector
  evaluation and seekable candidate-stream decisions; shorthand selectors
  tolerate whitespace around comparison operators, and brace selector
  assignments accept comma, newline, or whitespace-separated `key=value`
  clauses;
- selector parse-error parity tests cover supported-term key validation,
  duplicate-key validation, strict `in.any` value whitespace validation, and
  invalid selector invariants; C-native parser tests exercise those invalid
  expression classes through both public selector parser entry points,
  `ctx->selector_parse()` and `ctx->selector_parse_or()`;
- C-native selector parser contract tests now assert observable equivalence for
  alias spellings, assignment ordering, quoted `in.any` values, whitespace and
  multiline separators, shorthand comparison spacing, and nested wrapper
  aliases by evaluating each accepted form against matching and rejecting JSON;
- C-native selector evaluator contract tests now assert that `contains.any` and
  `icontains.any` behave like the corresponding explicit OR expressions across
  first-value matches, later-value matches, and no-match documents;
- `clql` exists as a selector/projection/mutation compatibility CLI with
  manifest-checked Go-backed parity coverage for selector composition,
  streaming selection, projection, mutation, inline/write modes, option
  parsing, malformed JSON diagnostics, and file-backed mutation workflows;
- fast CTest now includes `lql.cli-smoke`, which asserts the stable
  `clql --version` format, the documented `clql --help` option surface, and
  rejection of unsupported `prettyx` theme flags instead of accepting no-op
  compatibility options for features `clql` does not ship;
- C SDK contract tests cover the currently implemented public liblql selector,
  selector inspection, streaming, projection, compacting, mutation, version,
  and capability surfaces, including mutation plan parse success, expansion
  counts, default
  and explicit file-backed parse options, file-backed mutation value execution
  over buffered, source-backed, and seekable file-range APIs, source-backed
  projection, compacting, and mutation over fragmented caller reads, source
  callback read failures with empty failed-output state, wildcard remove
  mutation over object members, array-wildcard fields, any-child fields, and
  recursive descendants, mutation parse-error
  invariants,
  parser failure output-handle clearing, projection parser normalization for
  whitespace, blank fields, and duplicate paths, projection invalid-argument
  `out_found` state, and query callback failure status propagation. Query and
  candidate-stream mutation
  invalid-argument failures now also assert a safe zeroed `lql_query_result`
  output state. Decision and match callback failures now assert actionable
  diagnostics and partial result-counter propagation for seekable and
  callback-source query paths; callback-source decision and
  spooled match queries preserve partial result counters when a reader fails
  after an emitted candidate, matching the callback-source candidate mutation
  contract. Mixed scalar/object candidate-stream tests now cover both seekable
  and callback-source decision streams so non-object candidates reject non-empty
  selectors without being dropped from decision accounting.
  Projection-before-mutation candidate-stream tests cover both seekable and
  callback-source inputs in preserve-unmatched and matches-only modes.
  C-native streaming tests also cover recursive flattening of nested
  top-level array candidates for seekable decision streams, callback-source
  decision streams, seekable range payload streams, and callback-source spooled
  match payloads.
  Handle-producing selector, projection, and mutation APIs also have
  C-only ownership contract tests for optional diagnostics, output-handle
  clearing on parse failure, empty-selector ownership, and `NULL` cleanup/count
  behavior. An SDK coverage manifest ties claimed C contract surfaces to C
  unit functions. Further C test work should be driven by newly claimed API
  contracts or defects rather than by mechanically copying Go parity rows;
- Go-backed parity tests exist for the current CLI surface and remain a
  transitional oracle for semantic convergence; they run under the explicit
  `make parity-test` target and broader gates, not the fast `make test`
  target. They must not be counted as C SDK unit tests and must not drive
  mechanical duplication of Go rows into C tests. Their coverage manifests
  reject missing tests, duplicate test-function entries, and duplicate
  `surface/requirement` keys;
- C SDK and Lua facade tests now cover the public liblql equivalent of Go's
  fused query-mutate stream path: a fragmented callback source is filtered by a
  selector, only matched candidates are written, mutation is applied directly to
  those candidates, result counters report all candidates seen and matched, stop
  options can halt after a match limit, and time-prefixed mutation values
  normalize to UTC strings. The option-aware receiver method also preserves
  liblql's invalid-argument diagnostics and zeroed result contract. Go request
  construction conflicts and Go `OpenJSON`/inline payload-sink precedence are
  API-shape-specific unless liblql grows an equivalent public surface;
- Lockd-shaped local JSON engine handoff coverage now uses large fragmented
  callback-source candidates with selector-gated mutation, matches-only output,
  a two-match stop limit, and timestamp mutation. This covers liblql's public
  equivalent of the Go lockd stream integration tests without adopting the Go
  reusable payload-sink factory or its temp-file spill assertion as public C
  API;
- the pinned Go oracle inventory is executable through
  `parity/oracle_inventory.tsv` and `TestOracleInventory`; it currently records
  all `pkt.systems/lql v0.17.1` test, benchmark, and example-bearing files and
  classifies them as `covered` or `not-applicable` for the v0 public contract;
- CLI selector parity includes scalar, string, numeric, temporal, contains,
  prefix, existence, wildcard, recursive, numeric object/array segment,
  nested logical, and newline-separated selector cases from the Go oracle
  corpus, including multiple CLI selector arguments that combine ordinary
  AND terms with an embedded explicit OR group;
- JSON Pointer helper behavior from Go is not exposed as a standalone C helper
  API. The supported C contract is observable RFC 6901 path decoding through
  product surfaces: selector matching, streaming selector decisions,
  projection, and mutation all cover escaped slash and tilde segments such as
  `/a~1b/~0key`;
- C selector parse conformance covers the applicable public receiver string
  parser surface for brace assignment order, aliases, wrapper prefixes,
  whitespace, deep indexed merge behavior, and deep conflict rejection. Go's
  `url.Values` and slice-based selector parse entry points are not C public API
  unless a future C builder/config surface is deliberately added;
- C selector parser/evaluator coverage includes the remaining applicable
  public string-parser corpus from Go's selector tests: match-all aliases,
  empty-token handling, examples as observable AND/OR behavior, string-term
  empty-value simplification, contains/icontains any-value forms, in.any
  validation, shorthand range/date forms, explicit indexed merge/conflict
  behavior, and malformed parser regression inputs that must return normally
  without crashing;
- C temporal selector coverage is based on observable parse/evaluation
  behavior rather than Go's internal temporal state helpers. The C tests cover
  date-only equality, naive UTC datetimes, nanosecond precision, timezone
  offset normalization, temporal ranges, and stable current-date macro
  behavior through public selector matching;
- stream error/result coverage is based on liblql's public C contract rather
  than Go's typed error wrapping helpers. C exposes `lql_status`,
  `lql_status_string()`, `lql_error`, `lql_query_result`, and
  `LQL_STATUS_STOP`; C tests cover public status names, actionable diagnostics,
  safe zeroed result state on invalid arguments, callback-requested graceful
  stops, stop precedence, and partial result counters after malformed seekable
  and callback-source streams. Public result coverage also includes full-scan
  counts, consumed byte counts, decision-only streams, seekable and
  callback-source payload streams, and callback failure propagation. Go's
  `StreamError`, `AsStreamError`, `StreamErrorCodeOf`, `ErrStreamStop`
  wrapping behavior, capture-policy `BytesCaptured`, `SpillCount`, and
  `SpillBytes` result fields are not mirrored unless liblql deliberately grows
  equivalent public typed-error or aggregate accounting APIs;
- reusable query payload sink behavior is a caller-owned callback contract in
  C, not a public temp-file sink factory. C tests cover seekable payload ranges
  and callback-source spooled payloads written into caller-managed sinks,
  callback failure propagation, source-position preservation for seekable
  payload writes, and caller reset/reuse of the same sink state across payload
  streams. Go's reusable sink factory pooling and spill-file path lifecycle are
  implementation details unless liblql adds a factory API;
- reusable mutation payload sink behavior from Go is API-shape-specific.
  liblql's C mutation APIs do not expose a mutation `OnValue` callback,
  `DisableInternalSpool`, or a mutation payload-sink factory; they stream
  mutated bytes to caller-owned `FILE *` outputs. C tests cover the public
  behavior at that boundary: direct writer success, callback-source candidate
  mutation, matches-only output, stop limits, empty failed-output state for
  early source failures, partial output after a late read failure, and
  file-backed mutation values. Go callback-sink cleanup and reusable spill-file
  lifecycle are not mirrored unless liblql deliberately adds a mutation
  callback/factory surface;
- mutation stream result coverage follows the same public-boundary rule. C
  result tests cover seekable and callback-source candidate mutation counts,
  consumed bytes, writer output equivalence, match-limit stops, matches-only
  output, projected mutation results, malformed/read-failure partial counters,
  and invalid-argument zeroing through the public `lql_query_result` fields.
  Go's mutation callback stop/error tests and `BytesCaptured`/`SpillCount`/
  `SpillBytes` counters belong to Go's callback request shape and are not
  mirrored unless liblql grows an equivalent mutation callback/factory or
  aggregate accounting API;
- callback-source mutation stream coverage now includes mixed scalar/object
  candidates inside a root array: root-array items are emitted incrementally,
  object candidates are mutated, scalar candidates pass through unchanged, and
  partial read failures retain already-emitted candidate counters. The
  remaining Go `MutateStream` mixed-framing case is narrower: a root array
  followed by additional top-level values in the same callback source. lonejson
  `v0.35.2` exposes `AUTO`, `NDJSON`, `SINGLE_VALUE`, and `ARRAY_ITEMS`
  framing, but not a no-materialization mode that both emits root-array items
  incrementally and then continues with subsequent top-level values. liblql
  must not fake this by materializing the whole array or source; support for
  that exact shape needs dependency API support plus an explicit future public
  liblql framing contract that preserves streaming semantics. This is tracked
  as an accepted v0 non-parity case in `docs/liblql-dependency-gaps.md` and is
  not part of the current public liblql v0 callback-source contract;
- lonejson `v0.35.2` exposes candidate `stream_offset`, `byte_size`, and
  `payload_size` plus callback-scoped `lonejson_spooled` handles with
  per-handle size/spilled inspection. It does not expose aggregate query-level
  capture-byte, spill-count, or spill-byte counters. liblql must therefore not
  add `lql_query_result` aggregate spill counters by inference; doing so needs
  either a lonejson public accounting surface or an explicit not-applicable
  decision for Go's capture-policy counter fields;
- stream spool internals are not a standalone C product surface. liblql exposes
  callback-scoped `LQL_PAYLOAD_SPOOLED` payload handles and operations on those
  handles, not a public spool object with configurable temp-file naming,
  in-memory byte access, spill counters, or cleanup methods. C tests cover the
  public behavior: spooled payload write, sink write, projection, nested
  candidate traversal through spooled replay, stop limits before payload
  callbacks, reader failure/over-capacity errors, and source-candidate mutation
  over spooled candidates. Go spool file permissions, file reuse, spill
  counters, and private cleanup lifecycle are not mirrored unless liblql adds a
  public spool/factory API;
- CLI malformed JSON execution is covered by Go-backed parity tests over stdin
  and seekable file inputs for selection, matches-only selection, compact
  output, projection, and mutation, with exit-code and diagnostic assertions;
- CLI pflag-compatible long boolean value forms are covered for existing
  boolean flags that affect observable behavior, including
  `--compact=true`, `--matches-only=true|false`, `--or=true|false`, and
  `--enable-file-mutations=true|false`;
- CLI pflag-compatible short option clusters are covered for supported
  shorthand flags, including boolean clusters, short boolean `=true|false`
  values, and clustered `-f`, `-m`, and `-t` value forms;
- CLI pflag-compatible `--` end-of-options handling is covered so
  dash-prefixed positional input paths can be selected after the terminator;
- CLI pflag-compatible interspersed option parsing is covered so projection
  flags before or after selector arguments produce the same selected output;
- CLI positional argument compatibility covers directory arguments as selector
  text rather than input files, matching the Go CLI split behavior;
- CLI non-inline mutation supports multiple seekable input files and mixed
  file/stdin inputs in argument order, matching the Go CLI's
  `splitMutationArgs` behavior while preserving inline mode's single-file
  restriction;
- CLI mutation parse-error parity covers root paths, zero increments, invalid
  time forms, disabled file-backed values, enabled invalid file-backed
  operation forms, and malformed brace shorthand with Go-oracle exit-code
  assertions;
- CLI inline mutation input rejection matches the Go CLI distinction between
  missing file path and invalid single-file input forms such as stdin or
  multiple files; inline and write-mode execution failures for empty or
  malformed JSON input preserve the original input file rather than replacing
  it with a failed temp output; inline and write-mode projection-before-mutation
  can also deliberately replace the input with an empty file when every
  candidate is dropped by missing projected fields;
- CLI projection-before-mutation parity covers the edge where all projected
  fields are missing and no output should be emitted, and the edge where a
  matched candidate is dropped because its post-mutation projection is missing
  while an unmatched candidate with the projected field is preserved;
- Go-backed SDK parity tests now exist for public `liblql` selector
  parse/evaluate behavior for AND and OR parser entry points, top-level
  comma/newline selector separators, selector capability/trait inspection
  behavior, projection parser failures, buffered, source-backed,
  and seekable file-range JSON projection including malformed JSON execution
  errors, mutation plan parsing including comma/newline-separated expression
  strings, mutation parse failures, buffered, source-backed, and
  seekable file-range JSON mutation including file-backed mutation values and
  explicit textfile invalid UTF-8/NUL rejection, escaped JSON Pointer mutation
  paths, numeric object/array segment behavior for selector evaluation and
  mutation, wildcard and recursive mutation paths, array wildcard value
  mutation, mutation execution errors where a later parent replacement must not
  mask an earlier wildcard increment type error, and malformed JSON execution
  errors, compact serialization, compact error behavior, and current streaming
  query behavior through the receiver C API, comparing
  `ctx->selector_parse()`, `ctx->selector_parse_or()`, selector
  capability/trait inspection after both AND and OR parser entry points,
  `ctx->matches_json()`, `ctx->project_json()`, `ctx->project_source()`,
  `ctx->project_file_range()`, `ctx->mutation_plan_parse()`,
  `ctx->mutation_plan_parse_with_options()`, `ctx->mutation_plan_count()`,
  `ctx->mutate_json()`, `ctx->mutate_file_range_root_fields()`,
  `ctx->mutate_file_range_paths()`,
  `ctx->mutate_file_range_candidates()`,
  `ctx->mutate_file_range_candidates_with_options()`,
  `ctx->mutate_file_range_projected_candidates()`,
  `ctx->mutate_file_range_projected_candidates_with_options()`,
  `ctx->mutate_source_paths()`, `ctx->mutate_source_candidates()`,
  `ctx->mutate_source_candidates_with_options()`,
  `ctx->mutate_source_projected_candidates()`,
  `ctx->mutate_source_projected_candidates_with_options()`, `ctx->compact_json()`,
  `ctx->compact_source()`,
  `ctx->compact_file_range()`, `ctx->query_file_decisions()`,
  `ctx->query_source_decisions()`, `ctx->query_file_matches()`, and
  `ctx->query_source_spooled_matches()` against the pinned Go library or
  standard compact JSON behavior over the current selector, projection
  success/error, mutation success/error, compact success/error, and stream
  corpora; this is behavioral oracle coverage, not C SDK unit coverage and not
  a requirement that the C API mirror Go API shape;
- Nested top-level array flattening is proven for `clql` stdin and seekable
  file selection, `ctx->query_file_decisions()`,
  `ctx->query_source_decisions()`, `ctx->query_file_matches()`, and
  `ctx->query_source_spooled_matches()`. Seekable recursion rereads nested
  candidate ranges with absolute-offset `pread()` range readers so the active
  parser cursor is not disturbed and no hidden candidate materialization is
  introduced; callback-source recursion uses callback-scoped spooled payloads;
- C SDK projection tests assert duplicate projection paths are
  idempotent and parent/child projection path conflicts are rejected through
  the public projection API;
- C SDK projection tests assert the current projection parser failure
  corpus, including empty, blank, root, missing-leading-slash, leading-index,
  and oversized-index field sets;
- C SDK compact tests assert the current malformed JSON corpus through
  buffered, source-backed, and seekable file-range public compaction APIs;
- C SDK streaming tests assert the current malformed JSON corpus across
  seekable file decision streams, seekable file payload streams,
  callback-source decision streams, and callback-source spooled payload streams;
- C SDK payload tests assert callback-scoped seekable and callback-source
  spooled payloads can be written through caller-managed sink callbacks,
  including sink failure propagation, source position restoration after
  successful seekable payload writes and failed seekable payload writes,
  unrepresentable seekable payload offset failure, seekable plus-value stream
  stop limits for max matches, max candidates, and max bytes, and partial
  result accounting;
- SDK streaming parity currently asserts candidate counts, match counts,
  consumed byte counts, stop state/reason, callback counts, seekable/spooled
  payload kinds, decoded matched payload JSON collected through the public
  payload sink API, plus-value stop limits, match-all string terms over mixed
  scalar/object candidate streams, and malformed JSON stream errors. Full
  non-stopped streams report consumed input bytes, including
  trailing delimiters, while early-stop streams retain candidate-end accounting
  for stop decisions;
- C SDK streaming tests assert the public candidate offset/size contract
  directly for both seekable and callback-source decision streams: leading
  whitespace, blank lines, and trailing delimiters affect candidate offsets and
  total bytes consumed, but each `lql_query_decision.size` is the byte length
  of the JSON value itself;
- C SDK unit coverage is manifest-checked: every `expect_* (void)` SDK
  unit group in `tests/test_lql.c` must have exactly one manifest entry and
  exactly one `main()` call, so C-only regressions cannot be added without
  executable coverage accounting; `lql.sdk-manifest-fixtures` proves missing
  receiver method coverage, unmanifested SDK units, duplicate manifest entries,
  duplicate requirement keys, and duplicate main calls are rejected;
- C SDK mutation tests assert ordered wildcard error precedence: an earlier
  wildcard increment that matches a non-numeric descendant is still evaluated
  and reported when a later parent replacement would otherwise skip the
  original subtree; C SDK tests also assert recursive wildcard and array
  wildcard mutation behavior through the caller-buffered `ctx->mutate_json()`
  surface, not only through seekable file-range mutation;
- the randomized Go mutation parity row is covered in C by deterministic
  mutation invariants rather than by a random loop: root set/remove/increment,
  nested path creation, object-member and array-element wildcards, recursive
  descendants, numeric object/array path segments, array value replacement,
  and ordered wildcard error precedence are all asserted through public C
  mutation APIs. The Go random oracle remains useful during development, but
  C unit coverage should stay falsifiable and stable rather than depending on
  pseudo-random corpus generation;
- C SDK selector tests assert omitted-value string selectors
  (`contains`/`icontains`/`prefix`/`iprefix`) act as path-existence assertions
  across object, array, and null values, including wildcard path variants, while
  explicit empty-string root string selectors remain match-all and their
  negation remains never-match;
- Lua direct-core smoke tests assert the same omitted-value string selector
  contract through `client:matches_json()`, so the Lua facade proves this
  behavior through public liblql receiver calls rather than relying on `clql` or
  Go parity fixtures;
- Lua direct-core smoke tests assert projection field normalization through
  `client:project_json()`, including whitespace trimming, blank-field elision,
  duplicate-field idempotence, structured blank-only field-set errors, and
  projection-plan validation before selector-match filtering so invalid
  projection inputs are not hidden by non-matching buffered JSON candidates;
- Lua direct-core smoke tests assert recursive wildcard path mutations and
  array-wildcard value mutations through `client:mutate_json()`, proving those
  mutation semantics through the direct public liblql binding rather than
  through `clql` or Go parity fixtures;
- C SDK mutation tests assert explicit file-backed text values reject invalid
  UTF-8 and NUL bytes at execution time with actionable diagnostics instead of
  writing invalid JSON string content;
- bounded fuzz smoke coverage exists through `lql.fuzz-smoke` and
  `make fuzz-smoke`, exercising public selector parse/evaluate, projection,
  compaction, mutation, callback-source decision streams, and callback-source
  spooled payload streams with deterministic malformed and valid seeds; this is
  a local regression/sanitizer seed gate, not a replacement for long-running
  fuzz campaigns;
- the Go-backed SDK parity suite is intentionally excluded from sanitizer CTest
  presets because the cgo test process cannot reliably load an
  ASan-instrumented shared liblql with the ASan runtime first; project-owned C
  unit tests remain the sanitizer authority for SDK behavior;
- the Lua host smoke test is also excluded from sanitizer CTest presets because
  the system Lua executable loads before the ASan runtime; non-sanitizer
  `make test`, `make lua-test`, package verification, and release Lua artifact
  checks remain the Lua facade authorities;
- the Lua tree includes a Lua 5.5 facade over a direct `lql.core` C module
  linked against shared liblql and implemented through public liblql headers;
  `lql.new()` returns a C-owned client userdata backed by a public `lql *`
  receiver, with deterministic smoke tests for receiver version and capability
  queries, selector decisions, selection output, parsed AND and OR selector
  userdata reuse, parsed projection userdata reuse, parsed mutation-plan
  userdata reuse and count inspection over the public receiver surface,
  selector capability and execution-trait inspection, file-backed callback
  decision streams, Lua callback-backed source decision
  streams, Lua callback-source selection, callback-scoped seekable payload
  handles, callback-scoped source-spooled payload handles,
  callback-scoped payload streaming through `match.write_json(callback)`,
  buffered-JSON, seekable file, and Lua callback-source compaction, file,
  buffered-JSON, and Lua callback-source projection, file, buffered-JSON, and
  Lua callback-source mutation, relative file-backed mutation values through
  explicit `enable_file_mutations` and `file_value_base_dir` options, invalid
  UTF-8 and NUL rejection for explicit `textfile:` mutation values, query stop
  reasons, candidate/match limit options, expired payload handles,
  oversized Lua source-read chunk rejection across query, selection,
  match-payload, projection, and mutation source facades, source-read error
  propagation for query, selection, compaction, projection, and mutation,
  callback error propagation, selector AST JSON import/export, recursive
  selector AST inspection, all selector builders, selector userdata methods,
  selector builder/import error paths, and structured errors; the public API style gate rejects
  Lua facade use of private liblql headers, `LQL_INTERNAL_SYMBOL`, or private
  `_impl` receiver implementation functions so Lua remains a public-header
  binding rather than a private in-process shortcut; CMake verifies Lua headers
  are Lua 5.5 before enabling the direct module, Lua CTest smoke uses only a
  Lua 5.5 executable, and `lql.lua-runtime-fixtures` proves the standalone Lua
  test runner rejects non-5.5 runtimes with an actionable error;
- the parity benchmark surface now has Go, C, and Lua runners over the shared
  generated fixture matrix; the Lua runner loads `lua/lql.lua` and uses the
  direct `lql.core` module rather than shelling out to `clql`; Go helper
  records, C native helper records, and Lua facade runner records now report
  `ns_per_op`, Lua plus-value benchmark modes count bytes through
  `match.write_json(callback)` instead of `match.json()` materialization, Lua
  plan and reuse benchmark modes pass parsed selector userdata through the
  public facade rather than reparsing expression strings inside the timed
  operation, while selector parse-cost modes distinguish parsed selector reuse
  from reparsing expression strings on every timed run across Go, C, and Lua,
  and while
  Go and C helper records also report OS `getrusage` peak RSS as
  `peak_rss_bytes` for the benchmark schema and Lua records report process peak
  RSS when host `time` support is available; `make bench-check` enforces a
  supported-C smoke RSS ceiling through `LQL_BENCH_MAX_C_PEAK_RSS_BYTES`
  defaulting to 128 MiB; `make bench-memory-check` adds a separate scalable
  Go/C/Lua streaming profile that generates at least 16 MiB of NDJSON by
  default, compares C and Lua counters against Go, requires Lua peak RSS, and
  can be scaled with `LQL_BENCH_MEMORY_COUNT` and
  `LQL_BENCH_MEMORY_BLOB_BYTES`; `make bench-1g-check` is the explicit
  1 GiB/128 MiB profile over the same runner and validator, defaults to at
  least 1 GiB of generated NDJSON, and is part of the final `make release`
  gate because bounded-memory streaming is a product contract rather than an
  optional hardening check; the C allocator contract test also freezes the
  receiver allocator after warmup and rejects liblql-owned allocation attempts
  in warmed decision-query and seekable-match hot paths, including root-array
  file range flattening and a mixed selector that exercises exact,
  contains-any, prefix, numeric range, and temporal observers together, plus a
  nested container-observer selector that exercises `exists`, container
  `contains`, and container `prefix` without falling back to scalar-only
  observer branches; the same allocator gate freezes the receiver after
  projection warmup and seekable candidate mutation warmup, proving those
  supported hot paths do not allocate through the receiver in steady state;
- mutation execution derives a private literal-only path trait at plan parse
  time and uses it to bypass recursive virtual-key matching for common literal
  mutation plans such as nested set operations, reducing branches in the
  mutation visitor without introducing candidate/result caching or hidden
  materialization;
- source-backed projection and mutation are now profiled as dominated by
  lonejson candidate spooling/replay for dense non-seekable streams. The
  required final performance step is a lonejson single-pass candidate transform
  visitor that lets selector observation and projection/mutation writing share
  one validated parse; liblql must not emulate that with full-candidate
  buffering, temp-file staging, selector/result caches, or a second JSON
  parser. The dependency need is tracked in
  `docs/lonejson-cr-single-pass-candidate-transform.md` and summarized in
  `docs/liblql-dependency-gaps.md`;
- current local lifecycle confidence has passed `make test-all`,
  `make bench-check`, `make bench-memory-check`, `make bench-1g-check`,
  `make package-verify`, `make release-matrix`, and clean `make release` on
  the available host/toolchain set. The release matrix builds and verifies all
  Linux GNU/musl targets in the configured matrix.
  `LQL_PACKAGE_TARGETS=arm64-apple-darwin make release-matrix` also builds and
  verifies the Darwin arm64 `liblql` and `clql` artifacts when the osxcross
  compiler, linker, strip, and otool are available. lonejson `v0.35.2` does
  not publish an x86_64 Darwin SDK archive, so x86_64 Darwin is not a current
  liblql package target under the GitHub-release dependency boundary.
  These gates are strong evidence for the current implementation state, but
  they are not a substitute for a requirement-by-requirement completion audit
  before claiming full LQL parity or final release readiness;
- `make clean` removes generated `build/`, `dist/`, dependency cache, top-level
  Lua module output, and Lua object files while preserving Lua source files;
  `lql.clean-fixtures` proves this generated-state cleanup contract;
- `lql.cmake-presets` verifies required debug, debug-lua, sanitizer, and
  release target presets, base dependency-mode defaults, release target
  identity variables, build preset mirrors, and debug/asan test presets;
  `lql.cmake-presets-fixtures` proves missing Lua presets, wrong dependency
  defaults, wrong release target identity, and missing release build presets
  are rejected;
- `make release` is the final local release gate. It cleans generated state,
  runs the full local test gate, benchmark smoke gate, scalable memory
  benchmark gate, release target matrix, package verification, and checksum
  manifest verification. The `lql.release-surface` CTest check rejects a
  placeholder release target, verifies the project warning policy includes
  `-Werror`, verifies project-owned compiled targets apply that policy, and is
  included in source-archive verification so extracted release sources preserve
  the same lifecycle surface; `lql.release-surface-fixtures` proves the gate
  fails when `-Werror` or project target warning application is removed;
- host `liblql` and `clql` package archive production exists through
  `scripts/package.sh`, with checksum, layout, privacy, and ELF runtime-path
  verification, artifact-local lonejson dependency provenance manifests, shared
  and static install-tree CMake consumer smoke tests, shared and static
  pkg-config consumer smoke tests, and
  extracted host direct, CMake `find_package`, and pkg-config consumer smokes;
  fast CTest includes package privacy negative fixtures proving the verifier
  fails on repository paths, `$HOME`, absolute local `file://` URLs, and
  absolute ELF RPATH/RUNPATH entries when host tooling can create the fixture,
  and fails closed when `file(1)` or Linux ELF `readelf` inspection is
  unavailable for binary artifact verification; fast CTest also includes
  Mach-O negative fixtures proving the verifier fails closed when target-correct
  `otool` is unavailable for a Darwin package and rejects local or non-`@rpath`
  `liblql` install names, non-system absolute Darwin dependency paths, and
  non-relative Darwin rpaths; package verification now uses
  `scripts/discover_target_tools.sh` to resolve target inspection tools from
  the configured release build directory before ambient `PATH`, and
  `lql.target-tools-fixtures` covers configured CMake cache tools,
  target-prefixed compiler siblings, unprefixed compiler siblings, `PATH`
  fallback, and refusal to accept known host Darwin inspection tools for
  cross-built artifacts; package generation installs without CMake's ambient
  `--strip` shortcut and strips project-owned Linux installed binaries/shared
  libraries with the discovered target strip tool, with negative fixture
  coverage for missing strip and positive fixture coverage proving the selected
  strip is invoked. Darwin final artifacts are not stripped as routine package
  post-processing, avoiding Mach-O code-signature invalidation risk and keeping
  install-name correctness in the build/install graph. Target compiler/linker
  probes and CMake release builds run with the selected target tool directory
  prepended to `PATH`; for Darwin/osxcross builds package generation also sets
  `CMAKE_LINKER` to the target `${CPKT_OSXCROSS_HOST}-ld` and injects an
  absolute `-fuse-ld=<target-ld>` into executable, shared-library, and module
  linker flags. `lql.package-tool-path-fixtures` proves the Darwin link smoke
  route sees the target linker before ambient host tools and receives the
  explicit `-fuse-ld` argument;
  checksum manifest fixture coverage proves release-looking tarball, rockspec,
  and source-rock artifacts under `dist/` cannot be left out of the upload
  manifest, and `scripts/package.sh print-release-assets` derives the exact
  publish asset path list from the checksum manifest plus the manifest itself
  rather than from a `dist/` glob;
- standalone Lua source package production now writes
  `dist/liblql-lua-<version>.tar.gz` with `VERSION`, exact
  `RELEASE_MANIFEST`, Lua sources, tests, benchmark runner, rockspec template,
  Lua release scripts, and the Lua 5.5 runtime-contract fixture; `make
  release-lua-artifacts` also renders
  `dist/liblql-<version>-1.rockspec` with a public release URL and builds
  `dist/liblql-<version>-1.src.rock` through LuaRocks from the staged source
  package. Package verification checks checksum coverage, layout, manifest
  exactness, absence of C SDK payloads, local path privacy, release-safe
  rockspec URLs, and the nested Lua source package inside the source rock;
  package verification also rejects Lua release rockspecs that do not require
  `lua >= 5.5, < 5.6` and Lua source packages whose C module lacks the
  compile-time Lua 5.5 guard, with negative fixture coverage in
  `lql.package-lua-contract-fixtures`;
- source archive production exists with injected `VERSION`, `RELEASE_MANIFEST`,
  exact payload manifest verification, current git tracked-file manifest
  verification when package verification runs in a git worktree, and
  extracted-tree configure/build/test smoke; `lql.package-source-manifest-fixtures`
  proves stale source archive manifests are rejected;
- host `clql` archive production ships a single executable and verifies that
  `clql` has no dynamic `liblql` or `liblonejson` dependency. On Linux release
  builds request a static executable when the target compiler supports it; on
  Darwin, `liblql` and lonejson are linked into `clql` while normal system
  libraries remain dynamic. `clql` dependency provenance marks lonejson as a
  bundled static runtime component and package verification requires the
  bundled lonejson license, while the liblql SDK manifest marks lonejson as an
  external SDK dependency and checks the CMake/pkg-config dependency
  declarations;
- `make release-matrix` selects target-correct Linux compilers, acquires the
  matching lonejson SDK archive for each target, builds and verifies
  `liblql` and `clql` artifacts for `x86_64`, `aarch64`, and `armhf`
  GNU/musl targets, and fails package verification if packaged shared
  libraries or `clql` binaries do not match their target architecture.
  `arm64-apple-darwin` packaging uses the CMake Darwin system preset, an
  explicit `@rpath` install name for SDK `liblql`, a static `clql`
  project/dependency closure with only normal system dynamic libraries, no
  routine final-artifact Mach-O mutation, explicit osxcross target-linker
  routing through `PATH`, `CMAKE_LINKER`, and `-fuse-ld`, and target-correct
  `otool` verification when the osxcross toolchain is available.

The repository must not claim full LQL parity until the verification gates prove
it.

## Non-goals

- Do not reimplement `pkt.systems/prettyx` colorized JSON.
- Do not vendor the adjacent Go source checkout.
- Do not add hidden full-message buffering behind streaming-looking APIs.
- Do not implement bespoke JSON parser/tokenizer/serializer logic in liblql.
- Do not hide unsupported behavior by omitting tests or benchmark cases.
