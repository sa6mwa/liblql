#!/usr/bin/env python3
import json
import os
import subprocess
import sys
import tempfile


CLQL = os.environ.get("CLQL_PATH", "build/release/clql")
GO_LQL = os.environ.get("LQL_GO_CLI_PATH", "build/reference-lql")


def compact(obj):
    return json.dumps(obj, separators=(",", ":")) + "\n"


EXPECTED_DIVERGENCES = {
    (compact({"a": 1}), ("/a/b=3", "rm:/a/b")): [{"a": {}}],
    (compact({"a": None}), ("/a/b=3", "rm:/a/b")): [{"a": {}}],
    (compact({"a": "x"}), ("/a/b=3", "rm:/a/b")): [{"a": {}}],
    (compact({"a": 1}), ("/a/b=3", "/a/b=+1", "rm:/a/b")): [{"a": {}}],
    (compact({"a": None}), ("/a/b=3", "/a/b=+1", "rm:/a/b")): [{"a": {}}],
    (compact({"a": "x"}), ("/a/b=3", "/a/b=+1", "rm:/a/b")): [{"a": {}}],
}


def fail(message):
    print(f"clql mutation parity: {message}", file=sys.stderr)
    sys.exit(1)


def run_case(input_text, mutations):
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", delete=False) as f:
        f.write(input_text)
        path = f.name
    try:
        args = []
        for mutation in mutations:
            args.extend(["-m", mutation])
        c = subprocess.run(
            [CLQL, *args, path], text=True, capture_output=True, check=False
        )
        g = subprocess.run(
            [GO_LQL, "-c", *args, path],
            text=True,
            capture_output=True,
            check=False,
        )
    finally:
        os.unlink(path)
    return c, g


def parse_json_lines(stdout):
    records = []
    for line in stdout.splitlines():
        if line:
            records.append(json.loads(line))
    return records


def assert_case(name, input_text, mutations):
    c, g = run_case(input_text, mutations)
    c_records = None
    g_records = None
    try:
        if c.returncode == 0:
            c_records = parse_json_lines(c.stdout)
        if g.returncode == 0:
            g_records = parse_json_lines(g.stdout)
    except json.JSONDecodeError as exc:
        fail(
            f"{name} emitted invalid JSON: {exc}\n"
            f"  input: {input_text.strip()}\n"
            f"  mutations: {mutations!r}\n"
            f"  C rc={c.returncode} stdout={c.stdout!r} stderr={c.stderr!r}\n"
            f"  Go rc={g.returncode} stdout={g.stdout!r} stderr={g.stderr!r}"
        )
    expected_divergence = EXPECTED_DIVERGENCES.get((input_text, tuple(mutations)))
    if expected_divergence is not None:
        if c.returncode != 0 or c_records != expected_divergence:
            fail(
                f"{name} intentional divergence regressed\n"
                f"  input: {input_text.strip()}\n"
                f"  mutations: {mutations!r}\n"
                f"  expected C records={expected_divergence!r}\n"
                f"  C rc={c.returncode} stdout={c.stdout!r} stderr={c.stderr!r}\n"
                f"  Go rc={g.returncode} stdout={g.stdout!r} stderr={g.stderr!r}"
            )
        return
    if c.returncode != 0 and g.returncode != 0:
        return
    if c.returncode != g.returncode or c_records != g_records:
        fail(
            f"{name} mismatch\n"
            f"  input: {input_text.strip()}\n"
            f"  mutations: {mutations!r}\n"
            f"  C rc={c.returncode} stdout={c.stdout!r} stderr={c.stderr!r}\n"
            f"  Go rc={g.returncode} stdout={g.stdout!r} stderr={g.stderr!r}"
        )

def main():
    for binary, label in ((CLQL, "clql"), (GO_LQL, "Go lql")):
        if not os.path.exists(binary) or not os.access(binary, os.X_OK):
            fail(f"missing {label} binary: {binary}")

    explicit_cases = [
        ("missing nested increment", compact({}), ["/a/b=+1"]),
        ("empty object nested increment", compact({"a": {}}), ["/a/b=+1"]),
        (
            "nested increment appends to object",
            compact({"a": {"c": 2}}),
            ["/a/b=+1"],
        ),
        (
            "nested increment replaces scalar parent",
            compact({"a": None}),
            ["/a/b=+1"],
        ),
        ("mixed set then increment", compact({}), ["/a=2", "/a=+1"]),
        (
            "array nested remove",
            compact({"arr": [{"x": 1}, {"x": 2}]}),
            ["rm:/arr/0/x"],
        ),
        (
            "array nested increment",
            compact({"arr": [{"x": 1}, {"x": 2}]}),
            ["/arr/0/x=+1"],
        ),
        ("delete alias", compact({"a": 1, "b": 2}), ["delete:/a"]),
        ("del alias", compact({"a": 1, "b": 2}), ["del:/a"]),
        (
            "time literal mutation",
            compact({}),
            ["time:/timestamp=2025-01-01T00:00:00+02:00"],
        ),
        (
            "brace shorthand mutation",
            compact({}),
            ["/tags{/kind=document,/source=local}"],
        ),
        (
            "brace shorthand quoted comma",
            compact({"state": {"details": {"owner": "bob"}, "metrics": 1}}),
            [
                "/state/progress=ready",
                "/state/metrics++",
                '/state/details{/owner="alice",/note="hi, world"}',
                "/state/metrics=+3",
                "rm:/state/legacy",
            ],
        ),
        ("decrement shorthand", compact({"state": {"count": 5}}), ["/state/count--"]),
        (
            "array wildcard set",
            compact({"items": [{"sku": "a"}, {"sku": "b"}]}),
            ["/items/[]/seen=true"],
        ),
        (
            "array compact wildcard set",
            compact({"items": [{"sku": "a"}, {"sku": "b"}]}),
            ["/items[]/seen=true"],
        ),
        (
            "array wildcard increment",
            compact({"items": [{"n": 1}, {"n": 2}]}),
            ["/items/[]/n=+1"],
        ),
        (
            "array star wildcard increment",
            compact({"items": [{"n": 1}, {"n": 2}]}),
            ["/items/*/n=+1"],
        ),
        (
            "array recursive wildcard increment",
            compact({"items": [{"n": 1}, {"n": 2}]}),
            ["/items/**/n=+1"],
        ),
        (
            "object wildcard set",
            compact({"labels": {"a": {"x": 1}, "b": {"x": 2}}}),
            ["/labels/*/seen=true"],
        ),
        (
            "object wildcard increment",
            compact({"labels": {"a": {"n": 1}, "b": {"n": 2}}}),
            ["/labels/*/n=+1"],
        ),
        (
            "object recursive wildcard increment",
            compact({"labels": {"a": {"n": 1}, "b": {"n": 2}}}),
            ["/labels/**/n=+1"],
        ),
        (
            "recursive wildcard set",
            compact({"items": [{"parts": [{"sku": "a"}]}]}),
            ["/items/**/sku=patched"],
        ),
        (
            "ellipsis recursive wildcard set",
            compact({"items": [{"parts": [{"sku": "a"}], "sku": "top"}]}),
            ["/items/.../sku=patched"],
        ),
        (
            "ellipsis recursive wildcard increment",
            compact({"items": [{"parts": [{"n": 1}], "n": 2}]}),
            ["/items/.../n=+1"],
        ),
        (
            "wildcard remove",
            compact({"items": [{"x": 1, "y": 2}, {"x": 3}]}),
            ["rm:/items/[]/x"],
        ),
        (
            "wildcard remove combo",
            compact(
                {
                    "labels": {"env": "prod", "owner": "alice"},
                    "items": [{"sku": "A", "price": 10}, {"sku": "B", "price": 20}],
                    "nested": {"items": [{"sku": "C", "price": 30}]},
                }
            ),
            ["rm:/labels/*", "rm:/items[]/price", "rm:/items/**/sku", "rm:/nested/.../price"],
        ),
        (
            "wildcard full mutation combo",
            compact(
                {
                    "labels": {"env": "prod", "owner": "alice"},
                    "items": [{"sku": "A", "price": 10}, {"sku": "B", "price": 20}],
                    "groups": [{"items": [{"sku": "C"}]}],
                }
            ),
            [
                '/labels/*="tagged"',
                '/items/**/sku="X"',
                "/items[]/price=+5",
                "/items/*/price=+1",
                '/groups/.../sku="Z"',
            ],
        ),
    ]
    for name, input_text, mutations in explicit_cases:
        assert_case(name, input_text, mutations)

    # Mutation counts are public CLI input, not a scanner-bitset contract.
    # Exercise more actions than either a 32- or 64-bit word can represent.
    wide_count = 129
    assert_case(
        "mutations beyond one machine word",
        compact({}),
        [f"/k{i}={i}" for i in range(wide_count)],
    )

    objects = [
        {},
        {"a": 1},
        {"a": {"b": 1, "c": 2}},
        {"a": None},
        {"a": "x"},
        {"a": {"b": {"c": 2}}},
        {"x": 1, "a": {"b": 2}},
        {"arr": [{"x": 1}, {"x": 2}]},
    ]
    single_mutations = [
        "/a=2",
        "/a=x",
        "/a=true",
        "/a=null",
        "/a=+1",
        "/a++",
        "rm:/a",
        "delete:/a",
        "del:/a",
        "/a/b=3",
        "/a/b=+1",
        "rm:/a/b",
        "/a/b/c=z",
        "rm:/arr/0/x",
        "/arr/0/x=+1",
        "/arr/[]/x=3",
        "rm:/arr/[]/x",
    ]
    sequences = [
        ["/a=2", "/a=+1"],
        ["/a=2", "rm:/a"],
        ["rm:/a", "/a/b=3"],
        ["/a/b=3", "/a/b=+1"],
        ["/a/b=+1", "/a/b=+1"],
        ["/a/b=3", "rm:/a/b"],
        ["rm:/a/b", "/a/b/c=z"],
        ["/a=2", "/a/b=+1"],
        ["/a/b=+1", "/a=2"],
        ["/a=2", "/a=+1", "/a/b=3"],
        ["/a/b=3", "/a/b=+1", "rm:/a/b"],
        ["/arr/0/x=+1", "rm:/arr/0/x"],
        ["rm:/arr/0/x", "/arr/0/y=3"],
        ["/arr/[]/x=+1", "rm:/arr/[]/x"],
    ]
    count = 0
    for obj in objects:
        input_text = compact(obj)
        for mutation in single_mutations:
            count += 1
            assert_case("single mutation sweep", input_text, [mutation])
        for sequence in sequences:
            count += 1
            assert_case("ordered mutation sweep", input_text, sequence)

    print(
        f"clql mutation parity: {len(explicit_cases) + count + 1} cases passed"
    )


if __name__ == "__main__":
    main()
