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
    ]
    input_text = "\n".join(compact(record) for record in records) + "\n"

    cases = [
        ("shorthand eq quoted", ["/status=\"open\""]),
        ("shorthand neq", ["/status!=closed"]),
        ("shorthand numeric gte", ["/progress>=50"]),
        ("shorthand datetime gte", ["/timestamp>=\"2025-01-01T00:00:00Z\""]),
        ("array index shorthand", ["/items/0/sku=\"ABC-123\""]),
        ("object wildcard shorthand", ["/labels/*=\"production\""]),
        ("array wildcard shorthand", ["/items[]/sku=\"ABC-123\""]),
        ("recursive wildcard shorthand", ["/items/**/sku=\"ABC-123\""]),
        ("ellipsis wildcard shorthand", ["/items/.../sku=\"ABC-123\""]),
        ("contains value", ["contains{field=/msg,value=timeout}"]),
        ("contains value assignment order", ["contains{value=timeout,field=/msg}"]),
        ("contains value multiline assignments", ["contains{\nfield=/msg\nvalue=timeout\n}"]),
        ("contains any", ["contains{field=/msg,any=timeout|degraded}"]),
        ("contains alias any", ["contains{field=/msg,a=timeout|degraded}"]),
        ("contains ignore case bool alias", ["contains{field=/msg,value=TIMEOUT,ignoreCase=t}"]),
        ("contains omitted value", ["contains{field=/msg}"]),
        ("icontains value", ["icontains{field=/service,value=edge}"]),
        ("icontains alias any", ["icontains{field=/service,a=AUTH|EDGE}"]),
        ("prefix value", ["prefix{field=/service,value=AUTH}"]),
        ("iprefix value", ["iprefix{field=/service,value=auth}"]),
        ("prefix omitted value", ["prefix{field=/service}"]),
        ("range full", ["range{field=/progress,gte=50,lt=80}"]),
        ("date after before", ["date{field=/timestamp,after=2025-01-01,before=2025-02-01}"]),
        ("date aliases", ["date{f=/timestamp,a=2025-01-01,b=2025-02-01}"]),
        ("in any", ["in{field=/status,any=open|queued}"]),
        ("in any phrases", ["in{field=/greeting,any=\"hello world|goodbye jupiter\"}"]),
        ("exists bare", ["exists{/metadata/etag}"]),
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

    print(f"clql selector parity: {len(cases)} cases passed")


if __name__ == "__main__":
    main()
