#!/usr/bin/env python3
import os
import pathlib
import shutil
import sys


TOOLS = {
    "CC": ("LQL_CC", "CMAKE_C_COMPILER", None),
    "AR": ("LQL_AR", "CMAKE_AR", "ar"),
    "RANLIB": ("LQL_RANLIB", "CMAKE_RANLIB", "ranlib"),
    "STRIP": ("LQL_STRIP", "CMAKE_STRIP", "strip"),
    "NM": ("LQL_NM", "CMAKE_NM", "nm"),
    "OBJCOPY": ("LQL_OBJCOPY", "CMAKE_OBJCOPY", "objcopy"),
    "OBJDUMP": ("LQL_OBJDUMP", "CMAKE_OBJDUMP", "objdump"),
    "READELF": ("LQL_READELF", "CMAKE_READELF", "readelf"),
    "OTOOL": ("LQL_OTOOL", "CMAKE_OTOOL", "otool"),
    "INSTALL_NAME_TOOL": (
        "LQL_INSTALL_NAME_TOOL",
        "CMAKE_INSTALL_NAME_TOOL",
        "install_name_tool",
    ),
}


def fail(message: str) -> None:
    print(f"discover-target-tools: {message}", file=sys.stderr)
    sys.exit(1)


def parse_cache(path: pathlib.Path) -> dict:
    cache = {}
    if not path.exists():
        fail(f"missing CMake cache: {path}")
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith(("//", "#")) or "=" not in line:
            continue
        key_type, value = line.split("=", 1)
        key = key_type.split(":", 1)[0]
        cache[key] = value
    return cache


def executable(path: str) -> bool:
    return bool(path) and pathlib.Path(path).is_file() and os.access(path, os.X_OK)


def host_prefix(cc: str, target_id: str) -> str:
    name = pathlib.Path(cc).name
    for suffix in ("-gcc", "-cc", "-clang"):
        if name.endswith(suffix):
            return name[: -len(suffix)]
    if target_id == "arm64-apple-darwin":
        return os.environ.get("CPKT_OSXCROSS_HOST", "arm64-apple-darwin25")
    return ""


def sibling(cc: str, tool: str, target_id: str) -> str:
    if not cc:
        return ""
    directory = pathlib.Path(cc).parent
    prefix = host_prefix(cc, target_id)
    candidates = []
    if prefix:
        candidates.append(directory / f"{prefix}-{tool}")
    candidates.append(directory / tool)
    for candidate in candidates:
        if executable(str(candidate)):
            return str(candidate)
    return ""


def path_tool(tool: str) -> str:
    value = shutil.which(tool)
    return value or ""


def discover(name: str, cache: dict, target_id: str) -> str:
    override, cmake_key, fallback = TOOLS[name]
    if os.environ.get(override):
        value = os.environ[override]
        if not executable(value):
            fail(f"{override} is not executable: {value}")
        return value
    value = cache.get(cmake_key, "")
    if executable(value):
        return value
    cc = cache.get("CMAKE_C_COMPILER", "")
    if fallback:
        value = sibling(cc, fallback, target_id)
        if value:
            return value
        value = path_tool(fallback)
        if value:
            return value
    if name in ("OTOOL", "INSTALL_NAME_TOOL") and target_id != "arm64-apple-darwin":
        return ""
    fail(f"unable to discover {name} for {target_id}")
    return ""


def main(argv: list) -> int:
    if len(argv) != 3:
        fail("usage: discover_target_tools.py <build-dir> <target-id>")
    build_dir = pathlib.Path(argv[1])
    target_id = argv[2]
    cache = parse_cache(build_dir / "CMakeCache.txt")
    if cache.get("LQL_TARGET_ID") and cache["LQL_TARGET_ID"] != target_id:
        fail(
            f"target mismatch: cache has {cache['LQL_TARGET_ID']}, requested {target_id}"
        )
    values = {}
    for name in TOOLS:
        values[name] = discover(name, cache, target_id)
    values["TARGET_ID"] = target_id
    for key in sorted(values):
        if values[key]:
            print(f"{key}={values[key]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
