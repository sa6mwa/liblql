#!/usr/bin/env python3
import json
import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
PRESETS = ROOT / "CMakePresets.json"
MAKEFILE = ROOT / "Makefile"


REQUIRED_CONFIGURE_PRESETS = {
    "base",
    "debug",
    "debug-lua",
    "release",
    "valgrind",
    "fuzz",
    "x86_64-linux-gnu-release",
    "x86_64-linux-musl-release",
    "aarch64-linux-gnu-release",
    "aarch64-linux-musl-release",
    "armhf-linux-gnu-release",
    "armhf-linux-musl-release",
    "arm64-apple-darwin-release",
}

REQUIRED_BUILD_PRESETS = REQUIRED_CONFIGURE_PRESETS - {"base"}
REQUIRED_TEST_PRESETS = {"debug", "debug-lua", "valgrind"}
REQUIRED_MAKE_TARGETS = {
    "help",
    "deps-debug",
    "deps-release",
    "deps-cross",
    "build",
    "build-debug",
    "build-release",
    "test",
    "test-debug",
    "test-all",
    "valgrind",
    "package",
    "package-source",
    "package-source-smoke",
    "package-checksums",
    "package-verify",
    "verify-release-archives",
    "verify-release-privacy",
    "release-matrix",
    "prerelease",
    "prerelease-hardening",
    "prerelease-live",
    "release",
    "direct-parity-smoke",
    "direct-parity-matrix",
    "direct-profile-hotspots",
    "bench-gate",
    "perf-gate",
    "clean",
    "clean-dist",
}
OLD_PHASE = "scanner"
FORBIDDEN_PUBLIC_TERMS = {
    "debug-" + OLD_PHASE,
    "release-" + OLD_PHASE,
    OLD_PHASE + "-parity",
    OLD_PHASE + "-profile",
    "build/release-" + OLD_PHASE,
    "LQL_" + OLD_PHASE.upper() + "_",
}
LINUX_RELEASE_TARGETS = {
    "x86_64-linux-gnu-release": "x86_64-linux-gnu",
    "x86_64-linux-musl-release": "x86_64-linux-musl",
    "aarch64-linux-gnu-release": "aarch64-linux-gnu",
    "aarch64-linux-musl-release": "aarch64-linux-musl",
    "armhf-linux-gnu-release": "armhf-linux-gnu",
    "armhf-linux-musl-release": "armhf-linux-musl",
}


def fail(message: str) -> None:
    print(f"lifecycle preset check: {message}", file=sys.stderr)
    sys.exit(1)


def cache_vars(preset: dict) -> dict:
    value = preset.get("cacheVariables", {})
    if not isinstance(value, dict):
        fail(f"preset {preset.get('name')} has non-object cacheVariables")
    return value


def main() -> None:
    data = json.loads(PRESETS.read_text(encoding="utf-8"))
    configure = {preset["name"]: preset for preset in data.get("configurePresets", [])}
    build = {preset["name"]: preset for preset in data.get("buildPresets", [])}
    tests = {preset["name"]: preset for preset in data.get("testPresets", [])}

    missing = REQUIRED_CONFIGURE_PRESETS - set(configure)
    if missing:
        fail(f"missing configure presets: {', '.join(sorted(missing))}")
    missing = REQUIRED_BUILD_PRESETS - set(build)
    if missing:
        fail(f"missing build presets: {', '.join(sorted(missing))}")
    missing = REQUIRED_TEST_PRESETS - set(tests)
    if missing:
        fail(f"missing test presets: {', '.join(sorted(missing))}")

    text = PRESETS.read_text(encoding="utf-8") + "\n" + MAKEFILE.read_text(encoding="utf-8")
    forbidden = ["asan", "ubsan", "libfuzzer", "sanitize=address"]
    for word in forbidden:
        if word in text.lower():
            fail(f"forbidden sanitizer/libFuzzer lifecycle reference remains: {word}")
    for word in FORBIDDEN_PUBLIC_TERMS:
        if word in text:
            fail(f"legacy public lifecycle reference remains: {word}")

    phony_line = ""
    for line in MAKEFILE.read_text(encoding="utf-8").splitlines():
        if line.startswith(".PHONY:"):
            phony_line = line
            break
    if not phony_line:
        fail("Makefile is missing .PHONY command surface")
    phony_targets = set(phony_line.split()[1:])
    missing_targets = REQUIRED_MAKE_TARGETS - phony_targets
    if missing_targets:
        fail(f"missing lifecycle Make targets: {', '.join(sorted(missing_targets))}")

    base_vars = cache_vars(configure["base"])
    if base_vars.get("CMAKE_TOOLCHAIN_FILE") != "${sourceDir}/cmake/cpkt-toolchain.cmake":
        fail("base preset does not use cmake/cpkt-toolchain.cmake")
    if base_vars.get("LQL_TARGET_ID") != "x86_64-linux-gnu":
        fail("base preset does not default to x86_64-linux-gnu")

    fuzz_vars = cache_vars(configure["fuzz"])
    if fuzz_vars.get("CMAKE_TOOLCHAIN_FILE") != "${sourceDir}/cmake/cpkt-aflpp-toolchain.cmake":
        fail("fuzz preset does not use cmake/cpkt-aflpp-toolchain.cmake")
    if fuzz_vars.get("LQL_BUILD_FUZZERS") != "ON":
        fail("fuzz preset does not enable LQL_BUILD_FUZZERS")

    lua_vars = cache_vars(configure["debug-lua"])
    if lua_vars.get("LQL_BUILD_LUA") != "ON":
        fail("debug-lua preset does not enable LQL_BUILD_LUA")

    for preset_name, target_id in LINUX_RELEASE_TARGETS.items():
        if cache_vars(configure[preset_name]).get("LQL_TARGET_ID") != target_id:
            fail(f"{preset_name} does not set LQL_TARGET_ID={target_id}")

    darwin_vars = cache_vars(configure["arm64-apple-darwin-release"])
    if darwin_vars.get("LQL_TARGET_ID") != "arm64-apple-darwin":
        fail("arm64 Darwin release preset does not set LQL_TARGET_ID=arm64-apple-darwin")

    print("lifecycle preset check passed")


if __name__ == "__main__":
    main()
