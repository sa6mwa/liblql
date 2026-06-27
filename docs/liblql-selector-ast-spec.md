# liblql Selector AST Specification

## Purpose

This document defines the selector AST surface required for liblql parity with
the Go `pkt.systems/lql v0.17.1` public library. It is an implementation target,
not a record of the current code.

The goal is not to copy Go structs into C. The goal is to expose the same
public selector capabilities in idiomatic C:

- parse textual selector language into a public AST;
- construct selector ASTs without text parsing;
- inspect and traverse parsed or constructed ASTs;
- serialize and parse selector AST JSON compatible with Go;
- evaluate selectors and derive optimized plans from the same AST;
- expose the same high-value AST workflows through Lua.

The current opaque selector-evaluation handle is not enough for SDK parity.

## Public Model

`lql_selector` is the public selector AST owner. It may remain ABI-opaque, but
the installed header must expose receiver methods that make the tree observable
and constructible.

AST nodes are exposed as borrowed cursors or views. They are valid only while
the owning `lql_selector` remains alive and unchanged. Simple traversal must
not allocate.

The public node-kind enum must include:

- `match_all` or `empty`;
- `and`;
- `or`;
- `not`;
- `eq`;
- `contains`;
- `icontains`;
- `prefix`;
- `iprefix`;
- `range`;
- `date`;
- `in`;
- `exists`.

The public API must expose these node operations in receiver form:

- get selector root node;
- get node kind;
- get child count for `and` and `or`;
- get indexed child for `and` and `or`;
- get the single child for `not`;
- get string-term details for string predicates;
- get range-term details;
- get date-term details;
- get in-term details;
- get exists path.

The API should prefer out-parameters and borrowed string views:

```c
typedef struct lql_string_view {
  const char *data;
  size_t len;
} lql_string_view;
```

Owned strings returned by liblql need ownership-specific cleanup methods on the
owning receiver or selector. Downstream callers must never free liblql-owned
memory directly.

## Construction

The public API must support AST construction without parsing text. A builder or
node-factory API is acceptable as long as ownership is explicit and receiver
based.

The construction surface must support every public Go selector family:

- empty selector;
- `and` and `or` from child selector lists;
- `not` from one child;
- `eq`, `contains`, `icontains`, `prefix`, `iprefix` string terms;
- `range` terms with numeric or datetime bounds;
- `date` terms;
- `in` terms;
- `exists` path selectors.

Construction must preserve term details needed for Go-compatible JSON and
evaluation:

- field path;
- whether `value` was explicitly present;
- value text, including explicit empty value;
- `any` list;
- `ignoreCase`;
- numeric-vs-datetime range bound kind;
- raw datetime bound text;
- date raw strings;
- date `since` macro/literal state.

Builder validation must reject invalid combinations at the same public semantic
boundary as Go. Implementation-only caches or compiled programs must be derived
after construction, not required for tree ownership.

## JSON Shape

Selector AST JSON must be compatible with the Go public selector
representation as a structural interchange format:

```json
{
  "and": [],
  "or": [],
  "not": {},
  "eq": {"field": "/path", "value": "x"},
  "contains": {"field": "/path", "any": ["a", "b"]},
  "icontains": {"field": "/path", "value": "x"},
  "prefix": {"field": "/path", "value": "x"},
  "iprefix": {"field": "/path", "value": "x"},
  "range": {"field": "/n", "gte": 10, "lt": 20},
  "date": {"field": "/ts", "since": "today"},
  "in": {"field": "/env", "any": ["prod", "stage"]},
  "exists": "/meta/etag"
}
```

Serialization omits empty fields the same way Go omits `omitempty` fields. The
zero-value selector serializes as an empty JSON object. JSON object member order
is not a compatibility contract. The required contract is that Go-emitted
selector AST JSON parses into liblql, liblql-emitted selector AST JSON parses
as the same logical AST, and the resulting selector behavior is equivalent.

String terms have a critical invariant:

- omitted `value` means path assertion behavior for string predicates;
- explicit `"value": ""` means an explicit empty string value;
- non-empty `value` is emitted even when a constructor did not separately mark
  it as explicit.

JSON parsing must preserve that distinction.

Range bounds are a union:

- JSON numbers become numeric bounds;
- JSON strings become datetime text bounds after Go-compatible trimming;
- empty datetime string bounds are rejected;
- unsupported JSON types are rejected.

Term `value` parsing must accept the Go-compatible scalar forms and convert
them to selector strings. Term `any` parsing must accept both Go-compatible
string forms and arrays where the Go library accepts them.

## Lonejson Requirement

Selector AST JSON parsing and serialization must use lonejson `v0.35.0`.
liblql must not implement bespoke JSON parsing or escaping for selector AST
payloads.

Required lonejson usage:

- use lonejson value visitors for recursive selector sum-type parsing and JSON
  union points that fixed mappings cannot express directly;
- use lonejson mapped structs where they improve fixed-shape term parsing, but
  do not force recursive AST union handling through maps if a visitor is the
  clearer lonejson-native surface;
- use lonejson mapped serialization or `lonejson_writer` APIs for output;
- route lonejson runtime allocation through the active `lql *` receiver
  allocator bridge;
- cleanup every mapped transport value through lonejson cleanup/reset APIs.

Range bound and permissive scalar-to-string term conversion are the important
union points. The recursive selector node itself is also a union point:
`and`/`or` carry arrays, `not` carries one child node, term operators carry
operator-specific objects, and `exists` carries a string. The implementation may
parse those shapes through lonejson visitors, `lonejson_json_value` visitors, or
another lonejson-native adapter. It must not inspect raw JSON bytes with ad hoc
token code.

Manual JSON concatenation is not allowed, including for tests and small
selector objects, unless the text is a fixed fixture that is not parsed or
serialized by liblql code.

## Lua Surface

Lua selector values must be userdata backed by the public liblql selector AST
API. The Lua binding must not contain its own selector parser, must not maintain
an independent Lua-owned AST tree, and must not call `clql`.

Lua must support:

- `lql.new()` receiver creation;
- parsing selector text to selector userdata;
- constructing selector ASTs through C-backed userdata builders or
  constructors;
- inspecting selector userdata as node kinds, children, and term fields through
  methods backed by the public C API;
- converting selector userdata to Go-compatible selector JSON;
- constructing selector userdata from Go-compatible selector JSON;
- using selector userdata in query, projection-before-mutation, and mutation
  workflows.

Lua table helpers may be added for DX, for example to build a selector from a
literal table or produce a table snapshot for assertions. Those helpers are
conversion facades over liblql selector userdata. They must not become the
authoritative AST representation, must not bypass public C constructors or JSON
parsing, and must not duplicate selector validation logic in Lua.

Lua 5.5 is the only supported Lua runtime for this repository.

## Verification

The selector AST work is not complete until all of these are executable gates:

- C-only tests for public AST traversal of parsed text selectors;
- C-only tests for builder construction of every node family;
- C-only tests for JSON parse and serialize behavior through public liblql APIs;
- C-only tests for omitted `value` versus explicit empty `value`;
- C-only tests for range-bound numeric/string union behavior;
- C-only tests for cleanup after failed AST JSON parse and failed builder calls;
- Go-vs-C SDK parity tests proving Go selector AST JSON imports into liblql
  and preserves selector behavior across representative node families;
- Go-vs-C SDK parity tests for constructor-equivalent ASTs where the C builder
  is the public equivalent of Go constructors;
- Lua tests for selector userdata construction, traversal, JSON round-trips,
  optional table conversion helpers if present, and use in query workflows;
- manifest rows in `parity/oracle_inventory.tsv` updated from `gap` or
  `partial` only when the cited tests prove the public behavior.

Evaluator parity and capability inspection do not satisfy the AST requirement.
They are downstream consumers of the public AST.
