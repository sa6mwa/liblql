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
    ]
    for name, input_text, mutations in explicit_cases:
        assert_case(name, input_text, mutations)

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
        "/a/b=3",
        "/a/b=+1",
        "rm:/a/b",
        "/a/b/c=z",
        "rm:/arr/0/x",
        "/arr/0/x=+1",
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

    print(f"clql mutation parity: {len(explicit_cases) + count} cases passed")


if __name__ == "__main__":
    main()
