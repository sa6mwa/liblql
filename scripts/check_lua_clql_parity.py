#!/usr/bin/env python3
import json
import os
import shutil
import subprocess
import sys
import tempfile


CLQL = os.environ.get("CLQL_PATH", "build/release/clql")
LUA_CLI = os.environ.get("LQL_LUA_CLI_PATH", "build/luarocks/bin/lql.lua")
LUA_TREE = os.environ.get("LQL_LUAROCKS_TREE", "build/luarocks")
LUA_SDK_PREFIX = os.environ.get("LQL_LUA_SDK_PREFIX", "build/lua-sdk")


def fail(message):
    print(f"lua clql parity: {message}", file=sys.stderr)
    sys.exit(1)


def compact(obj):
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)


def parse_json_lines(stdout):
    return [json.loads(line) for line in stdout.splitlines() if line]


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


def run_clql(args):
    return subprocess.run([CLQL, *args], text=True, capture_output=True, check=False)


def assert_exact_case(name, args):
    lua = run_lua(args)
    clql = run_clql(args)
    if lua.returncode != clql.returncode or lua.stdout != clql.stdout:
        fail(
            f"{name} output mismatch\n"
            f"  args={args!r}\n"
            f"  lua rc={lua.returncode} stdout={lua.stdout!r} stderr={lua.stderr!r}\n"
            f"  clql rc={clql.returncode} stdout={clql.stdout!r} stderr={clql.stderr!r}"
        )


def assert_json_file_equal(name, lua_path, clql_path):
    with open(lua_path, "r", encoding="utf-8") as f:
        lua_text = f.read()
    with open(clql_path, "r", encoding="utf-8") as f:
        clql_text = f.read()
    if parse_json_lines(lua_text) != parse_json_lines(clql_text):
        fail(f"{name} file mismatch\n  lua={lua_text!r}\n  clql={clql_text!r}")


def assert_failure_contains(name, args, needles):
    lua = run_lua(args)
    clql = run_clql(args)
    if lua.returncode == 0 or clql.returncode == 0:
        fail(
            f"{name} unexpectedly succeeded\n"
            f"  args={args!r}\n"
            f"  lua rc={lua.returncode} stderr={lua.stderr!r}\n"
            f"  clql rc={clql.returncode} stderr={clql.stderr!r}"
        )
    for needle in needles:
        if needle not in lua.stderr or needle not in clql.stderr:
            fail(
                f"{name} failure text mismatch for {needle!r}\n"
                f"  lua stderr={lua.stderr!r}\n"
                f"  clql stderr={clql.stderr!r}"
            )


def assert_help_cluster():
    lua = run_lua(["-vh"])
    clql = run_clql(["-vh"])
    if lua.returncode != 0 or clql.returncode != 0:
        fail(f"-vh did not succeed\n  lua={lua!r}\n  clql={clql!r}")
    for text, label in ((lua.stdout, "lua"), (clql.stdout, "clql")):
        if (
            "usage:" not in text
            or "Selector examples (full LQL):" not in text
            or "Notes:" not in text
            or "only date{...,since=...} supports relative macros" not in text
        ):
            fail(f"-vh missing help sections in {label} stdout: {text!r}")


def main():
    for binary, label in ((CLQL, "clql"), (LUA_CLI, "lql.lua")):
        if not os.path.exists(binary) or not os.access(binary, os.X_OK):
            fail(f"missing {label} binary: {binary}")

    tmpdir = tempfile.mkdtemp(prefix="liblql-lua-clql-parity.")
    try:
        fixture = os.path.join(tmpdir, "input.ndjson")
        records = [
            {"id": "a", "status": "new", "keep": 1, "labels": {"env": "prod"}},
            {"id": "b", "status": "old", "keep": 2, "labels": {"env": "stage"}},
            {"id": "c", "status": "new", "keep": 3, "labels": {"env": "prod"}},
        ]
        with open(fixture, "w", encoding="utf-8") as f:
            for record in records:
                f.write(compact(record) + "\n")

        exact_cases = [
            ("selection", ['/status="new"', fixture]),
            ("count", ["--count", '/status="new"', fixture]),
            ("cluster compact matches", ["-cM", '/status="new"', fixture]),
            (
                "long false compact matches",
                ["--compact=false", "--matches-only=false", '/status="new"', fixture],
            ),
            ("or", ["-O", '/status="new"', '/id="b"', fixture]),
            ("or false", ["--or=false", '/status="new"', '/id="c"', fixture]),
            ("field attached", ["-f/id", '/status="new"', fixture]),
            ("field equals", ["-f=/id", '/status="new"', fixture]),
            ("field interspersed", ['/status="new"', "-f", "/id", fixture]),
            ("mutation attached", ["-m/status=ready", '/id="b"', fixture]),
            ("mutation equals", ["-m=/status=ready", '/id="b"', fixture]),
            ("mutation interspersed", ['/id="b"', "-m", "/status=ready", fixture]),
            ("matches-only mutation", ["-M", "-m", "/status=ready", '/id="b"', fixture]),
            ("count mutation", ["--count", "-m", "/status=ready", '/id="b"', fixture]),
            (
                "projection mutation",
                ["-f", "/id", "-m", "/status=ready", '/id="b"', fixture],
            ),
            (
                "inline disabled bool",
                ["--inline=false", "-m", "/status=ready", '/id="b"', fixture],
            ),
        ]
        for name, args in exact_cases:
            assert_exact_case(name, args)

        text_path = os.path.join(tmpdir, "blob-utf8.txt")
        with open(text_path, "w", encoding="utf-8") as f:
            f.write("日本語 😀 こんにちは")
        binary_path = os.path.join(tmpdir, "blob.bin")
        with open(binary_path, "wb") as f:
            f.write(b"a\x00b\xff")
        assert_exact_case(
            "utf8 textfile",
            ["-F", "-M", "-m", f"textfile:/payload={text_path}", '/id="a"', fixture],
        )
        assert_exact_case(
            "base64file",
            ["-F", "-M", "-m", f"base64file:/payload={binary_path}", '/id="a"', fixture],
        )

        source = os.path.join(tmpdir, "inline-source.ndjson")
        lua_inline = os.path.join(tmpdir, "inline-lua.ndjson")
        clql_inline = os.path.join(tmpdir, "inline-clql.ndjson")
        shutil.copyfile(fixture, source)
        shutil.copyfile(source, lua_inline)
        shutil.copyfile(source, clql_inline)
        lua = run_lua(["-i", "-m", "/status=ready", '/id="b"', lua_inline])
        clql = run_clql(["-i", "-m", "/status=ready", '/id="b"', clql_inline])
        if lua.returncode != clql.returncode:
            fail(
                f"inline exit mismatch\n"
                f"  lua rc={lua.returncode} stderr={lua.stderr!r}\n"
                f"  clql rc={clql.returncode} stderr={clql.stderr!r}"
            )
        assert_json_file_equal("inline", lua_inline, clql_inline)

        assert_help_cluster()
        assert_failure_contains("invalid help cluster", ["-hZ"], ["unknown flag"])
        assert_failure_contains("invalid version cluster", ["-vZ"], ["unknown flag"])
        assert_failure_contains(
            "boolean yes rejected",
            ["--compact=yes", '/status="new"', fixture],
            ["invalid boolean value for --compact"],
        )
        assert_failure_contains(
            "boolean no rejected",
            ["--or=no", '/status="new"', fixture],
            ["invalid boolean value for --or"],
        )
        assert_failure_contains("theme unsupported", ["-t", "jq", fixture], ["prettyx"])
        assert_failure_contains(
            "file mutation disabled",
            ["-m", "file:/payload=blob.txt", fixture],
            ["file-backed mutations are disabled"],
        )
        assert_failure_contains(
            "inline count rejected",
            ["--count", "-i", "-m", "/status=ready", '/id="b"', fixture],
            ["inline mutation cannot be combined with --count"],
        )
        inline_target = os.path.join(tmpdir, "inline-target.ndjson")
        inline_link = os.path.join(tmpdir, "inline-link.ndjson")
        shutil.copyfile(fixture, inline_target)
        os.symlink("inline-target.ndjson", inline_link)
        assert_failure_contains(
            "inline symlink rejected",
            ["-i", "-m", "/status=ready", inline_link],
            ["inline mode does not rewrite symlink paths"],
        )
        inline_fifo = os.path.join(tmpdir, "inline-fifo.ndjson")
        os.mkfifo(inline_fifo)
        assert_failure_contains(
            "inline fifo rejected",
            ["-i", "-m", "/status=ready", inline_fifo],
            ["inline mode requires a single JSON file"],
        )
        selector_path = os.path.join(tmpdir, "selector-looking-file")
        with open(selector_path, "w", encoding="utf-8") as f:
            f.write("not a selector\n")
        inline_arg_target = os.path.join(tmpdir, "inline-arg-target.ndjson")
        shutil.copyfile(fixture, inline_arg_target)
        assert_failure_contains(
            "inline existing selector path counted as input",
            ["-i", "-m", "/status=ready", selector_path, inline_arg_target],
            ["inline mode requires a single JSON file"],
        )
    finally:
        shutil.rmtree(tmpdir)

    print("lua clql parity: 30 cases passed")


if __name__ == "__main__":
    main()
