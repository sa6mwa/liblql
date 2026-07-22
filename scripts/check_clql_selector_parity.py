#!/usr/bin/env python3
import json
import os
import subprocess
import sys
import tempfile


CLQL = os.environ.get("CLQL_PATH", "build/release/clql")
GO_LQL = os.environ.get("LQL_GO_CLI_PATH", "build/reference-lql")


def fail(message):
    print(f"clql selector parity: {message}", file=sys.stderr)
    sys.exit(1)


def compact(obj):
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)


def parse_json_lines(stdout):
    records = []
    for line in stdout.splitlines():
        if line:
            records.append(json.loads(line))
    return records


def run_case(input_text, args):
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", delete=False) as f:
        f.write(input_text)
        path = f.name
    try:
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


def assert_case(name, input_text, args):
    c, g = run_case(input_text, args)
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
            f"  args: {args!r}\n"
            f"  C rc={c.returncode} stdout={c.stdout!r} stderr={c.stderr!r}\n"
            f"  Go rc={g.returncode} stdout={g.stdout!r} stderr={g.stderr!r}"
        )
    if c.returncode != 0 and g.returncode != 0:
        return
    if c.returncode != g.returncode or c_records != g_records:
        fail(
            f"{name} mismatch\n"
            f"  args: {args!r}\n"
            f"  C rc={c.returncode} stdout={c.stdout!r} stderr={c.stderr!r}\n"
            f"  Go rc={g.returncode} stdout={g.stdout!r} stderr={g.stderr!r}"
        )


def main():
    for binary, label in ((CLQL, "clql"), (GO_LQL, "Go lql")):
        if not os.path.exists(binary) or not os.access(binary, os.X_OK):
            fail(f"missing {label} binary: {binary}")

    records = [
        {
            "id": "a",
            "status": "open",
            "single": "true",
            "count_text": "1",
            "progress": 75,
            "timestamp": "2025-01-15T10:00:00Z",
            "service": "AUTH-edge",
            "msg": "timeout on auth edge",
            "greeting": "hello world",
            "region": "us",
            "state": "enabled",
            "metadata": {"etag": "abc"},
            "labels": {"env": "production", "tier": "edge"},
            "items": [{"sku": "ABC-123"}, {"sku": "ZZZ"}],
            "nested": {"items": [{"sku": "DEEP"}]},
            "values": ["A", "B"],
            "emoji": "こんにちは 😀",
        },
        {
            "id": "b",
            "status": "queued",
            "progress": 40,
            "timestamp": "2025-02-10",
            "service": "billing",
            "msg": "all good",
            "greeting": "goodbye jupiter",
            "region": "eu",
            "state": "disabled",
            "labels": {"env": "staging"},
            "items": [{"sku": "NOPE"}],
            "values": ["C", "D"],
        },
        {
            "id": "c",
            "status": "closed",
            "progress": 50,
            "timestamp": "2024-12-31T23:59:59Z",
            "service": "EDGE-cache",
            "msg": "degraded cache",
            "greeting": "other",
            "region": "apac",
            "state": "enabled",
            "metadata": {},
            "labels": {"env": "production"},
            "items": [{"parts": [{"sku": "ABC-123"}]}],
            "values": ["B"],
        },
        {
            "id": "d",
            "status": "open,closed",
            "single": "not-true",
            "count_text": "not-1",
            "progress": 60,
            "timestamp": "2026-03-11T01:11:28.123456789Z",
            "service": "misc",
            "msg": "hello world",
            "greeting": "hello world",
            "region": "us",
            "state": "enabled",
            "metadata": {},
            "meta,etag": "comma",
            "labels": {"env": "test"},
            "items": [],
            "values": [],
        },
        {
            "status": "open",
            "timestamp": "2026-03-11T00:11:28.123Z",
            "service": "misc",
            "msg": "terminal ellipsis anchor",
            "greeting": "other",
            "region": "eu",
            "state": "enabled",
            "metadata": {},
            "labels": {},
            "items": ["ABC-123", {"sku": "other"}],
            "values": [],
            "a": "",
        },
        {
            "id": "e",
            "status": "time",
            "single": "true",
            "count_text": "1",
            "progress": 61,
            "timestamp": "2026-03-11T01:11:28.123+01:00",
            "service": "misc",
            "msg": "timezone fraction",
            "greeting": "other",
            "region": "eu",
            "state": "enabled",
            "metadata": {},
            "labels": {"env": "test"},
            "items": [],
            "values": [],
        },
        {
            "id": "f",
            "status": "time",
            "single": "false",
            "count_text": "1.0",
            "progress": 62,
            "timestamp": "2026-03-11T00:11:28.123Z",
            "service": "misc",
            "msg": "utc fraction",
            "greeting": "other",
            "region": "eu",
            "state": "enabled",
            "metadata": {},
            "labels": {"env": "test"},
            "items": [],
            "values": [],
        },
    ]
    input_text = "\n".join(compact(record) for record in records) + "\n"

    cases = [
        ("shorthand eq quoted", ["/status=\"open\""]),
        ("shorthand eq single quoted bool string", ["/single='true'"]),
        ("shorthand eq single quoted number string", ["/count_text='1'"]),
        ("shorthand neq", ["/status!=closed"]),
        ("shorthand numeric gte", ["/progress>=50"]),
        ("shorthand datetime gte", ["/timestamp>=\"2025-01-01T00:00:00Z\""]),
        ("array index shorthand", ["/items/0/sku=\"ABC-123\""]),
        ("object wildcard shorthand", ["/labels/*=\"production\""]),
        ("array wildcard shorthand", ["/items[]/sku=\"ABC-123\""]),
        ("recursive wildcard shorthand", ["/items/**/sku=\"ABC-123\""]),
        ("ellipsis wildcard shorthand", ["/items/.../sku=\"ABC-123\""]),
        ("terminal recursive wildcard shorthand", ["/**=\"\""]),
        ("terminal ellipsis wildcard shorthand", ["/...=\"\""]),
        ("terminal ellipsis anchor shorthand", ["/a/...=\"\""]),
        ("nested terminal ellipsis wildcard shorthand", ["/items/...=\"ABC-123\""]),
        ("ellipsis followed by object wildcard", ["/.../*=\"\""]),
        ("ellipsis followed by array index", ["/items/.../0=\"ABC-123\""]),
        ("ellipsis followed by array wildcard", ["/items/.../[]=\"ABC-123\""]),
        ("repeated ellipsis selector", ["/.../...=\"ABC-123\""]),
        ("wildcard between repeated ellipsis selector", ["/.../*/...=\"ABC-123\""]),
        ("contains value", ["contains{field=/msg,value=timeout}"]),
        ("contains value assignment order", ["contains{value=timeout,field=/msg}"]),
        ("contains value multiline assignments", ["contains{\nfield=/msg\nvalue=timeout\n}"]),
        ("eq single quoted comma", ["eq{field=/status,value='open,closed'}"]),
        ("contains single quoted phrase", ["contains{field=/msg,value='hello world'}"]),
        ("contains any", ["contains{field=/msg,any=timeout|degraded}"]),
        ("contains alias any", ["contains{field=/msg,a=timeout|degraded}"]),
        ("contains ignore case bool alias", ["contains{field=/msg,value=TIMEOUT,ignoreCase=t}"]),
        ("contains omitted value", ["contains{field=/msg}"]),
        ("contains empty value", ['contains{field=/msg,value=""}']),
        ("icontains empty value", ['icontains{field=/msg,value=""}']),
        ("icontains value", ["icontains{field=/service,value=edge}"]),
        ("icontains alias any", ["icontains{field=/service,a=AUTH|EDGE}"]),
        ("prefix value", ["prefix{field=/service,value=AUTH}"]),
        ("iprefix value", ["iprefix{field=/service,value=auth}"]),
        ("iprefix empty value", ['iprefix{field=/service,value=""}']),
        ("prefix omitted value", ["prefix{field=/service}"]),
        ("range full", ["range{field=/progress,gte=50,lt=80}"]),
        ("date after before", ["date{field=/timestamp,after=2025-01-01,before=2025-02-01}"]),
        ("date aliases", ["date{f=/timestamp,a=2025-01-01,b=2025-02-01}"]),
        ("date value", ["date{field=/timestamp,value=2025-02-10}"]),
        ("date gte lt", ["date{field=/timestamp,gte=2025-01-01,lt=2025-02-01}"]),
        ("datetime naive fraction", ["/timestamp=\"2026-03-11T01:11:28.123456789\""]),
        ("datetime timezone fraction", ["/timestamp=\"2026-03-11T00:11:28.123Z\""]),
        ("in any", ["in{field=/status,any=open|queued}"]),
        ("in any phrases", ["in{field=/greeting,any=\"hello world|goodbye jupiter\"}"]),
        ("exists bare", ["exists{/metadata/etag}"]),
        ("exists single quoted comma", ["exists{'/meta,etag'}"]),
        ("not eq", ["not.eq{field=/state,value=disabled}"]),
        (
            "and prefixed",
            ["and.eq{field=/status,value=open},and.range{field=/progress,gte=50}"],
        ),
        (
            "and indexed nested or",
            [
                "and.0.eq{field=/status,value=open},and.1.or.0.in{f=/region,a=us|eu},and.1.or.1.exists{/metadata/etag}"
            ],
        ),
        (
            "or prefixed",
            ["or.eq{field=/region,value=us},or.eq{field=/region,value=eu}"],
        ),
        ("cli or flag", ["-O", "/status=\"open\"", "/status=\"queued\""]),
        ("projection", ["-f", "/id", "-f", "/emoji", "/status=\"open\""]),
        ("invalid selector", ["contains{field=/msg,value=}"]),
    ]
    for name, args in cases:
        assert_case(name, input_text, args)

    numeric_container_input = "\n".join(
        compact(record)
        for record in [
            {"a": {"b": 3}},
            {"a": {"b": None}},
            {"a": {"b": False}},
            {"a": [3]},
        ]
    ) + "\n"
    assert_case("numeric shorthand ignores container", numeric_container_input, ["/a>=2"])
    assert_case(
        "numeric range ignores container",
        numeric_container_input,
        ["range{field=/a,gte=2}"],
    )
    recursive_wildcard_input = (
        compact({"0": {"a": [1, {"n": 2}]}, "items": "1", "a": [1, 2]}) + "\n"
    )
    assert_case(
        "recursive selector after object wildcard",
        recursive_wildcard_input,
        ["/*/...=1"],
    )
    assert_case(
        "recursive selector after array wildcard",
        compact({"items": [{"a": [None]}, {"a": [1]}]}) + "\n",
        ["/items[]/...=1"],
    )
    assert_case(
        "recursive selector through any and array wildcards",
        compact({"x": {"a": 1, "z": [1]}}) + "\n",
        ["/x/.../**/[]=1"],
    )
    assert_case(
        "recursive selector after named path does not overmatch",
        compact({"x": {"b": 1}}) + "\n",
        ["/.../x/b/...=1"],
    )
    assert_case(
        "recursive selector numeric object key before object wildcard",
        compact({"0": {"a": ""}}) + "\n",
        ["/.../0/*=\"\""],
    )
    assert_case(
        "recursive selector numeric object key nested below array",
        compact({"x": [{"0": {"a": ""}}, ""]}) + "\n",
        ["/.../0/*=\"\""],
    )
    assert_case(
        "recursive selector branch state does not leak into sibling values",
        compact({"a": {"star": [{"star": [1, True]}, [None]]}}) + "\n",
        ["/a/.../[]/0!=1"],
    )
    assert_case(
        "recursive selector preserves branch state across object siblings",
        compact({"a": {"0": {"a": "x", "0": {"0": "y"}}}}) + "\n",
        ["/.../0/0=\"y\""],
    )
    assert_case(
        "consecutive recursive selector keeps zero-depth match",
        compact({"b": "y"}) + "\n",
        ["/.../.../b=\"y\""],
    )
    assert_case(
        "terminal recursive exists includes empty root object",
        compact({}) + "\n",
        ["exists{/...}"],
    )
    assert_case(
        "terminal recursive exists includes root object with null child",
        compact({"a": None}) + "\n",
        ["exists{/...}"],
    )
    assert_case(
        "terminal recursive omitted prefix includes root object",
        compact({}) + "\n",
        ["prefix{field=/...}"],
    )
    assert_case(
        "terminal recursive omitted contains includes root object",
        compact({"a": None}) + "\n",
        ["contains{field=/...}"],
    )
    assert_case(
        "scalar root does not satisfy field inequality",
        "1\n" + compact({"a": False}) + "\n",
        ["/a!=true"],
    )

    # Scanner term state used to be represented by one machine word. Keep this
    # above both 32- and 64-bit widths so selector and projection parity cannot
    # regress to an implementation-only execution limit.
    wide_count = 129
    wide_record = {"id": "wide"}
    wide_record.update({f"k{i}": i for i in range(wide_count)})
    wide_input = compact(wide_record) + "\n"
    assert_case(
        "selectors beyond one machine word",
        wide_input,
        [f"/k{i}={i}" for i in range(wide_count)],
    )
    assert_case(
        "selector miss beyond one machine word",
        wide_input,
        [f"/k{i}={i}" for i in range(wide_count - 1)] + ["/k128=missing"],
    )
    assert_case(
        "wide shared in disjunct",
        compact({"status": "open"}) + "\n",
        [f"in{{field=/status,any=open|other-{i}}}" for i in range(wide_count)],
    )
    assert_case(
        "projection beyond one machine word",
        wide_input,
        sum((["-f", f"/k{i}"] for i in range(wide_count)), [])
        + ["/id=wide"],
    )
    long_needle = "a" * 300
    long_input = compact({"id": "long", "msg": "x" + long_needle + "y"}) + "\n"
    assert_case(
        "contains needle beyond old fast-path capacity",
        long_input,
        [f'contains{{field=/msg,value="{long_needle}"}}'],
    )
    assert_case(
        "icontains needle beyond old fast-path capacity",
        long_input,
        [f'icontains{{field=/msg,value="{long_needle.upper()}"}}'],
    )

    print(f"clql selector parity: {len(cases) + 13} cases passed")


if __name__ == "__main__":
    main()
