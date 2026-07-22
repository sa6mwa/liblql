#!/usr/bin/env python3
import json
import os
import shutil
import subprocess
import sys
import tempfile


GO_LQL = os.environ.get("LQL_GO_CLI_PATH", "build/reference-lql")
LUA_CLI = os.environ.get("LQL_LUA_CLI_PATH", "build/luarocks/bin/lql.lua")
LUA_TREE = os.environ.get("LQL_LUAROCKS_TREE", "build/luarocks")
LUA_SDK_PREFIX = os.environ.get("LQL_LUA_SDK_PREFIX", "build/lua-sdk")


def fail(message):
    print(f"lua cli parity: {message}", file=sys.stderr)
    sys.exit(1)


def compact(obj):
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)


def parse_json_lines(stdout):
    records = []
    for line in stdout.splitlines():
        if line:
            records.append(json.loads(line))
    return records


def lua_env():
    env = os.environ.copy()
    env["LUA_PATH"] = (
        f"{LUA_TREE}/share/lua/5.5/?.lua;"
        f"{LUA_TREE}/share/lua/5.5/?/init.lua;;"
    )
    env["LUA_CPATH"] = (
        f"{LUA_TREE}/lib/lua/5.5/?.so;"
        f"{LUA_TREE}/lib/lua/5.5/?/core.so;;"
    )
    ld = env.get("LD_LIBRARY_PATH", "")
    env["LD_LIBRARY_PATH"] = f"{LUA_SDK_PREFIX}/lib" + (f":{ld}" if ld else "")
    return env


def run_lua(args):
    return subprocess.run(
        [LUA_CLI, *args], text=True, capture_output=True, check=False, env=lua_env()
    )


def run_go(args):
    return subprocess.run(
        [GO_LQL, "-c", *args], text=True, capture_output=True, check=False
    )


def assert_json_case(name, input_text, args):
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", delete=False) as f:
        f.write(input_text)
        path = f.name
    try:
        lua = run_lua([*args, path])
        go = run_go([*args, path])
    finally:
        os.unlink(path)
    if lua.returncode != go.returncode:
        fail(
            f"{name} exit mismatch\n"
            f"  args={args!r}\n"
            f"  lua rc={lua.returncode} stdout={lua.stdout!r} stderr={lua.stderr!r}\n"
            f"  go rc={go.returncode} stdout={go.stdout!r} stderr={go.stderr!r}"
        )
    if lua.returncode != 0:
        return
    try:
        lua_records = parse_json_lines(lua.stdout)
        go_records = parse_json_lines(go.stdout)
    except json.JSONDecodeError as exc:
        fail(
            f"{name} emitted invalid JSON: {exc}\n"
            f"  args={args!r}\n"
            f"  lua stdout={lua.stdout!r}\n"
            f"  go stdout={go.stdout!r}"
        )
    if lua_records != go_records:
        fail(
            f"{name} JSON output mismatch\n"
            f"  args={args!r}\n"
            f"  lua stdout={lua.stdout!r}\n"
            f"  go stdout={go.stdout!r}"
        )
    for line in lua.stdout.splitlines():
        if line and line != compact(json.loads(line)):
            fail(f"{name} Lua output is not compact JSON: {line!r}")


def assert_lua_text_case(name, input_text, args, expected_stdout):
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", delete=False) as f:
        f.write(input_text)
        path = f.name
    try:
        lua = run_lua([*args, path])
    finally:
        os.unlink(path)
    if lua.returncode != 0 or lua.stdout != expected_stdout:
        fail(
            f"{name} Lua output mismatch\n"
            f"  args={args!r}\n"
            f"  lua rc={lua.returncode} stdout={lua.stdout!r} stderr={lua.stderr!r}\n"
            f"  expected={expected_stdout!r}"
        )


def assert_exact_stdout_case(name, input_text, args):
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", delete=False) as f:
        f.write(input_text)
        path = f.name
    try:
        lua = run_lua([*args, path])
        go = run_go([*args, path])
    finally:
        os.unlink(path)
    if lua.returncode != go.returncode or lua.stdout != go.stdout:
        fail(
            f"{name} exact output mismatch\n"
            f"  args={args!r}\n"
            f"  lua rc={lua.returncode} stdout={lua.stdout!r} stderr={lua.stderr!r}\n"
            f"  go rc={go.returncode} stdout={go.stdout!r} stderr={go.stderr!r}"
        )


def assert_inline_case(tmpdir):
    src = os.path.join(tmpdir, "inline.ndjson")
    lua_src = os.path.join(tmpdir, "inline.lua.ndjson")
    go_src = os.path.join(tmpdir, "inline.go.ndjson")
    with open(src, "w", encoding="utf-8") as f:
        f.write('{"id":"a","status":"new"}\n{"id":"b","status":"old"}\n')
    shutil.copyfile(src, lua_src)
    shutil.copyfile(src, go_src)
    args = ["-i", "-m", "/status=ready", '/id="b"']
    lua = run_lua([*args, lua_src])
    go = run_go([*args, go_src])
    if lua.returncode != go.returncode:
        fail(
            f"inline exit mismatch\n"
            f"  lua rc={lua.returncode} stderr={lua.stderr!r}\n"
            f"  go rc={go.returncode} stderr={go.stderr!r}"
        )
    with open(lua_src, "r", encoding="utf-8") as f:
        lua_out = f.read()
    with open(go_src, "r", encoding="utf-8") as f:
        go_out = f.read()
    if parse_json_lines(lua_out) != parse_json_lines(go_out):
        fail(f"inline file output mismatch\n  lua={lua_out!r}\n  go={go_out!r}")


def main():
    for binary, label in ((GO_LQL, "Go lql"), (LUA_CLI, "lql.lua")):
        if not os.path.exists(binary) or not os.access(binary, os.X_OK):
            fail(f"missing {label} binary: {binary}")

    records = [
        {
            "id": "a",
            "status": "open",
            "progress": 75,
            "service": "AUTH-edge",
            "msg": "timeout on auth edge",
            "labels": {"env": "production"},
            "items": [{"sku": "ABC-123"}, {"sku": "ZZZ"}],
            "emoji": "日本語 😀",
        },
        {
            "id": "b",
            "status": "queued",
            "progress": 40,
            "service": "billing",
            "msg": "all good",
            "labels": {"env": "staging"},
            "items": [{"sku": "NOPE"}],
        },
        {
            "id": "c",
            "status": "closed",
            "progress": 55,
            "service": "EDGE-cache",
            "msg": "degraded cache",
            "labels": {"env": "production"},
            "items": [{"parts": [{"sku": "ABC-123"}]}],
        },
    ]
    input_text = "\n".join(compact(record) for record in records) + "\n"

    json_cases = [
        ("selector eq", ["/status=\"open\""]),
        ("selector or", ["-O", "/status=\"open\"", "/status=\"queued\""]),
        ("selector contains", ["contains{field=/msg,value=timeout}"]),
        ("selector wildcard", ["/labels/*=\"production\""]),
        ("selector recursive", ["/items/**/sku=\"ABC-123\""]),
        ("projection", ["-f", "/id", "-f", "/status", "/status!=missing"]),
        ("mutation all", ["-m", "/seen=true"]),
        ("mutation matched only", ["-M", "-m", "/status=ready", '/id="b"']),
        (
            "projection then mutation",
            ["-f", "/id", "-m", "/status=ready", "/status!=missing"],
        ),
    ]
    for name, args in json_cases:
        assert_json_case(name, input_text, args)

    assert_exact_stdout_case("selected compact output", input_text, ["/status=\"open\""])
    assert_lua_text_case("count", input_text, ["--count", "/status!=missing"], "3\n")

    with tempfile.TemporaryDirectory(prefix="liblql-lua-parity.") as tmpdir:
        source = os.path.join(tmpdir, "payload.txt")
        input_path = os.path.join(tmpdir, "input.ndjson")
        with open(source, "w", encoding="utf-8") as f:
            f.write("日本語 😀")
        with open(input_path, "w", encoding="utf-8") as f:
            f.write('{"id":"a"}\n')
        lua = run_lua(["-F", "-m", f"textfile:/payload={source}", input_path])
        go = run_go(["-F", "-m", f"textfile:/payload={source}", input_path])
        if lua.returncode != go.returncode:
            fail(
                f"file-backed mutation exit mismatch\n"
                f"  lua rc={lua.returncode} stderr={lua.stderr!r}\n"
                f"  go rc={go.returncode} stderr={go.stderr!r}"
            )
        if parse_json_lines(lua.stdout) != parse_json_lines(go.stdout):
            fail(
                f"file-backed mutation output mismatch\n"
                f"  lua stdout={lua.stdout!r}\n"
                f"  go stdout={go.stdout!r}"
            )
        assert_inline_case(tmpdir)

    theme = run_lua(["--theme", "dark", "/status=\"open\""])
    if theme.returncode == 0 or "unsupported" not in theme.stderr:
        fail(
            f"theme unsupported contract regressed\n"
            f"  rc={theme.returncode} stdout={theme.stdout!r} stderr={theme.stderr!r}"
        )

    print(f"lua cli parity: {len(json_cases) + 4} cases passed")


if __name__ == "__main__":
    main()
