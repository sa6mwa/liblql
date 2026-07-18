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
            "nested brace shorthand mutation",
            compact({}),
            ["/a{/b{/c=1,/d=2},/e=3}"],
        ),
        ("empty set value", compact({}), ["/a="]),
        ("bracket comma value", compact({}), ["/a=[1,2]"]),
        ("bracket spaced comma value", compact({}), ["/a=[1, 2]"]),
        ("raw text bracket comma value", compact({}), ["/a=x[y,z]"]),
        ("raw text brace comma value", compact({}), ["/a=x{y,z}"]),
        ("raw text quote comma value", compact({}), ['/a=foo"bar,baz']),
        ("raw apostrophe value", compact({}), ["/a=don't"]),
        ("raw double quote value", compact({}), ['/a=a"b']),
        ("literal open brace path", compact({}), ["/a{=x"]),
        ("literal close brace path", compact({}), ["/a}=x"]),
        ("literal bracket path", compact({}), ["/a[=x"]),
        ("literal raw open brace value", compact({}), ["/a=x{y"]),
        ("literal raw close brace value", compact({}), ["/a=x}y"]),
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
            "array wildcard increment preserves scalar elements",
            compact({"items": [1, 2, {"n": 3}]}),
            ["/items[]/n=+1"],
        ),
        (
            "array wildcard increment preserves array elements",
            compact({"items": [[1, 2], {"n": 3}]}),
            ["/items[]/n=+1"],
        ),
        (
            "array wildcard increment indexes nested arrays",
            compact({"items": [{"0": 2}, [2]]}),
            ["/items[]/0=+1"],
        ),
        (
            "numeric nested increment materializes object key over array parent",
            compact({"0": {}, "a": [{"a": False, "0": ""}]}),
            ["/a/0=+1"],
        ),
        (
            "array wildcard increment applies nested array wildcard only to arrays",
            compact({"items": [[2], {"n": 3}]}),
            ["/items[]/[]=+1"],
        ),
        (
            "array wildcard increment recurses through object children",
            compact({"items": [{"a": {"n": 1}}, {"a": {"n": 2}}]}),
            ["/items[]/a/n=+1"],
        ),
        (
            "array wildcard increment recurses through array indexes",
            compact({"items": [[{"n": 1}], [{"n": 2}]]}),
            ["/items[]/0/n=+1"],
        ),
        (
            "array wildcard set updates nested array index",
            compact({"items": [[1, 2]]}),
            ["/items[]/0=5"],
        ),
        (
            "array wildcard set recurses through array indexes",
            compact({"items": [[{"n": 1}]]}),
            ["/items[]/0/n=5"],
        ),
        (
            "array wildcard remove preserves indexed array shape",
            compact({"items": [[1, 2]]}),
            ["rm:/items[]/0"],
        ),
        (
            "missing recursive wildcard set is no-op",
            compact({}),
            ["/obj/**/n=5"],
        ),
        (
            "missing ellipsis wildcard set is no-op",
            compact({}),
            ["/obj/.../n=5"],
        ),
        (
            "top object wildcard set",
            compact({"a": 1, "b": 2}),
            ["/*=3"],
        ),
        (
            "top object wildcard increment",
            compact({"a": 1, "b": 2}),
            ["/*=+1"],
        ),
        (
            "top object wildcard remove",
            compact({"a": 1, "b": 2}),
            ["rm:/*"],
        ),
        (
            "top object wildcard nested set preserves scalar",
            compact({"a": 1}),
            ["/*/n=5"],
        ),
        (
            "top object wildcard nested set existing only",
            compact({"x": {"a": {"n": 1}}, "y": [{"a": {"n": 2}}], "z": 1}),
            ["/*/a/n=5"],
        ),
        (
            "top object wildcard nested increment existing only",
            compact({"x": {"a": {"n": 1}}, "y": [{"a": {"n": 2}}], "z": 1}),
            ["/*/a/n=+1"],
        ),
        (
            "top ellipsis nested set recurses existing only",
            compact({"x": {"a": {"n": 1}}, "y": [{"a": {"n": 2}}], "z": 1}),
            ["/.../n=5"],
        ),
        (
            "top ellipsis set applies root key before descendants",
            compact({"b": {"b": 1}}),
            ["/.../b=5"],
        ),
        (
            "top ellipsis increment applies root key before descendants",
            compact({"b": 1}),
            ["/.../b=+1"],
        ),
        (
            "top ellipsis remove applies root key before descendants",
            compact({"b": {"b": 1}}),
            ["rm:/.../b"],
        ),
        (
            "ellipsis numeric segment selects array index before descendants",
            compact({"a": {"b": [[], [0]]}}),
            ["/a/.../0=5"],
        ),
        (
            "ellipsis numeric remove preserves array shape",
            compact({"a": {"b": [[], [0]]}}),
            ["rm:/a/.../0"],
        ),
        (
            "consecutive ellipsis set treats second ellipsis as wildcard",
            compact({"a": {"x": 1}}),
            ["/.../...=5"],
        ),
        (
            "consecutive ellipsis set collapses before named key",
            compact({"b": {"a": False}}),
            ["/.../.../b=x"],
        ),
        (
            "consecutive ellipsis remove treats second ellipsis as wildcard",
            compact({"a": {"x": 1}}),
            ["rm:/.../..."],
        ),
        (
            "ellipsis after wildcard set matches current object",
            compact({"b": {"b": 1}}),
            ["/*/.../b=x"],
        ),
        (
            "ellipsis after wildcard set matches current array index",
            compact({"a": [[False, 0, None]], "b": []}),
            ["/*/.../0=x"],
        ),
        (
            "recursive object wildcard set continues into descendants",
            compact({"r": {"x": {"a": "x"}}}),
            ["/.../*/a=null"],
        ),
        (
            "leading recursive object wildcard set matches zero depth",
            compact({"a": {"b": 2}}),
            ["/.../*/b=9"],
        ),
        (
            "leading recursive object wildcard increment matches zero depth",
            compact({"a": {"b": 2}}),
            ["/.../*/b++"],
        ),
        (
            "leading recursive object wildcard remove matches zero depth",
            compact({"a": {"b": 2}}),
            ["rm:/.../*/b"],
        ),
        (
            "recursive any wildcard set continues into array descendants",
            compact({"r": [{"a": "x"}]}),
            ["/.../**/a=null"],
        ),
        (
            "recursive object wildcard does not carry recursion after star",
            compact({"x": {"z": [1, {"a": 2}]}}),
            ["/x/.../*/*=v"],
        ),
        (
            "top wildcard before concrete set preserves mutation order",
            compact({"a": 0, "b": 0}),
            ["/*=1", "/a=2"],
        ),
        (
            "top concrete before wildcard set preserves mutation order",
            compact({"a": 0, "b": 0}),
            ["/a=2", "/*=1"],
        ),
        (
            "missing concrete key is affected by later wildcard set",
            compact({}),
            ["/a=2", "/*=1"],
        ),
        (
            "created concrete key is removed by later wildcard remove",
            compact({"a": 0, "b": 0}),
            ["/a=2", "rm:/*"],
        ),
        (
            "wildcard set replaces before later recursive increment validation",
            compact({"r": "x"}),
            ["/**=5", "/...=+1"],
        ),
        (
            "recursive concrete suffix does not materialize scalar parent",
            compact({"a": -1}),
            ["/.../a/1=+1"],
        ),
        (
            "later recursive terminal applies to earlier created key",
            compact({}),
            ["/b=+1", "/.../...=5"],
        ),
        (
            "repeated recursive segment before array wildcard stays meaningful",
            compact({"0": [True, {"a": 1}]}),
            ["/.../.../[]=5"],
        ),
        (
            "top wildcard nested action applies under matched key",
            compact({"a": {}}),
            ["/*/x=1", "/a/y=2"],
        ),
        (
            "top wildcard nested action does not recreate removed key",
            compact({"a": {}}),
            ["rm:/a", "/*/x=1"],
        ),
        (
            "terminal repeated ellipsis increment preserves recursion",
            compact({"obj": {"a": 1}}),
            ["/obj/.../...++"],
        ),
        (
            "terminal repeated ellipsis increment ignores scalar anchor",
            compact({"obj": 1}),
            ["/obj/.../...++"],
        ),
        (
            "missing increment array wildcard suffix is no-op",
            compact({"b": {}}),
            ["/b/0/[]=+1"],
        ),
        (
            "missing increment object wildcard suffix is no-op",
            compact({"b": {}}),
            ["/b/0/*=+1"],
        ),
        (
            "missing increment any wildcard suffix is no-op",
            compact({"b": {}}),
            ["/b/0/**=+1"],
        ),
        (
            "missing increment ellipsis suffix is no-op",
            compact({"b": {}}),
            ["/b/0/...=+1"],
        ),
        (
            "scalar parent object wildcard set is no-op",
            compact({"a": 1}),
            ["/a/*/n=x"],
        ),
        (
            "scalar parent array wildcard set is no-op",
            compact({"a": 1}),
            ["/a/[]/n=x"],
        ),
        (
            "scalar parent recursive wildcard set is no-op",
            compact({"a": 1}),
            ["/a/**/n=x"],
        ),
        (
            "scalar parent later object wildcard set is no-op",
            compact({"a": True}),
            ["/a/0/*=x"],
        ),
        (
            "scalar parent later object wildcard increment is no-op",
            compact({"a": True}),
            ["/a/0/*=+1"],
        ),
        (
            "scalar parent later array wildcard set is no-op",
            compact({"a": True}),
            ["/a/x/[]/n=1"],
        ),
        (
            "scalar parent later recursive wildcard set is no-op",
            compact({"a": True}),
            ["/a/x/**/n=1"],
        ),
        (
            "skipped wildcard permits later top set",
            compact({}),
            ["/a/*/n=x", "/a=2"],
        ),
        (
            "skipped wildcard permits later nested set",
            compact({}),
            ["/a/*/n=x", "/a/b=2"],
        ),
        (
            "skipped wildcard permits later increment",
            compact({}),
            ["/a/*=x", "/a++"],
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
            "terminal ellipsis recursive wildcard set",
            compact({"a": {"x": 1, "y": 2}}),
            ["/a/...=3"],
        ),
        (
            "terminal object wildcard increment",
            compact({"a": {"x": {"n": 1}}}),
            ["/a/*/**=+1"],
        ),
        (
            "terminal ellipsis wildcard increment",
            compact({"a": {"x": {"n": 1}}}),
            ["/a/*/...=+1"],
        ),
        (
            "terminal repeated recursive wildcard increment",
            compact({"a": {"x": {"n": 1}}}),
            ["/a/**/**=+1"],
        ),
        (
            "preserve array for later object wildcard increment",
            compact({"b": [1, 2]}),
            ["/b/x/*=+1"],
        ),
        (
            "preserve array for later array wildcard increment",
            compact({"b": [1, 2]}),
            ["/b/x/[]=+1"],
        ),
        (
            "preserve array for later recursive wildcard increment",
            compact({"b": [1, 2]}),
            ["/b/x/**=+1"],
        ),
        (
            "preserve array for later ellipsis wildcard increment",
            compact({"b": [1, 2]}),
            ["/b/x/...=+1"],
        ),
        (
            "signed negative zero increment is rejected",
            compact({"n": 1}),
            ["/n=-0"],
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
    assert_case(
        "mutations beyond one machine word with existing first key",
        compact({"k0": 0}),
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
