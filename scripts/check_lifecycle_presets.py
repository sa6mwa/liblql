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
    "toolchain-check",
    "toolchain-policy-check",
    "development-runtime-check",
    "dependency-cache-check",
    "dependency-cache-privacy-regression",
    "lifecycle-version-contract",
    "ctest-contract",
    "build",
    "build-debug",
    "build-release",
    "test",
    "test-debug",
    "shared-only-smoke",
    "cross-build",
    "cross-test",
    "test-all",
    "selector-ast-contract",
    "lua-test",
    "lua-rock",
    "lua-rock-toolchain-override-check",
    "lua-cli-smoke",
    "lua-artifact-privacy-regression",
    "release-lua-artifacts",
    "valgrind",
    "package",
    "package-source",
    "package-source-smoke",
    "source-manifest-exactness",
    "package-checksums",
    "package-verify",
    "release-upload-list",
    "verify-release-archives",
    "verify-release-privacy",
    "release-matrix",
    "prerelease",
    "prerelease-hardening",
    "prerelease-live",
    "release",
    "print-release-version",
    "sdk-parity-contract",
    "sdk-parity-gate",
    "direct-parity-matrix",
    "direct-perf-gate",
    "direct-profile-hotspots",
    "bench-gate",
    "perf-gate",
    "finalize-slice",
    "format",
    "clean",
    "clean-dist",
}
REQUIRED_SCRIPT_SURFACES = {
    "scripts/deps.sh",
    "scripts/build.sh",
    "scripts/test.sh",
    "scripts/host_test.sh",
    "scripts/cross_build.sh",
    "scripts/cross_test.sh",
    "scripts/fuzz.sh",
    "scripts/osxcross_available.sh",
    "scripts/check_darwin_linker_route.sh",
    "scripts/run_linux_release_matrix.sh",
    "scripts/release_version.sh",
    "scripts/stage_release_sources.sh",
    "scripts/test_release_from_source.sh",
    "scripts/verify_release_artifacts.sh",
    "scripts/verify_release_privacy.sh",
    "scripts/build_lua_rock.sh",
    "scripts/check_lua_rock_toolchain_override.sh",
    "scripts/render_release_rockspec.sh",
    "scripts/stage_lua_rock_sources.sh",
    "scripts/validate_luarocks.sh",
    "scripts/package-verify.sh",
    "scripts/print_release_uploads.sh",
    "scripts/print_release_version.sh",
    "scripts/check_source_manifest_exactness.sh",
    "scripts/check_shared_only_build.sh",
    "scripts/test_dependency_cache_config.sh",
    "scripts/check_dependency_cache_privacy_regression.sh",
    "scripts/check_toolchain_policy.sh",
    "scripts/check_development_runtime.sh",
    "scripts/check_lifecycle_version_contract.sh",
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
    if "-fuse-ld=" in text:
        fail("Darwin lifecycle linker must use --ld-path, not -fuse-ld=/path")
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

    missing_scripts = [
        script for script in sorted(REQUIRED_SCRIPT_SURFACES)
        if not (ROOT / script).is_file()
    ]
    if missing_scripts:
        fail(f"missing lifecycle script surfaces: {', '.join(missing_scripts)}")

    base_vars = cache_vars(configure["base"])
    if base_vars.get("CMAKE_TOOLCHAIN_FILE") != "${sourceDir}/cmake/cpkt-toolchain.cmake":
        fail("base preset does not use cmake/cpkt-toolchain.cmake")
    if base_vars.get("LQL_TARGET_ID") != "x86_64-linux-gnu":
        fail("base preset does not default to x86_64-linux-gnu")

    cmake_lists = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    if "include(LqlDependencyCache)" not in cmake_lists:
        fail("CMake does not resolve CPKT_DEPENDENCY_CACHE through LqlDependencyCache")
    if "include(LqlDevelopmentRuntime)" not in cmake_lists:
        fail("CMake does not configure the Bootlin development runtime helper")
    if "LIBLQL_TOOLCHAIN_OVERRIDE" not in cmake_lists:
        fail("CMake does not expose the explicit non-Bootlin toolchain override")
    if "CPKT_AFLPP_ACTIVE" not in cmake_lists:
        fail("CMake does not recognize the lifecycle-owned AFL++ compiler wrapper")
    if "stable-2026.08-1" not in (ROOT / "scripts/cpkt-toolchains.sh").read_text(encoding="utf-8"):
        fail("Bootlin resolver is not pinned to stable-2026.08-1")
    if "include(LqlLuaRuntime)" not in cmake_lists or "lql_lua_runner" not in cmake_lists:
        fail("Lua facade does not provide the Bootlin runtime runner")
    if "finalize-slice: format toolchain-policy-check development-runtime-check" not in MAKEFILE.read_text(encoding="utf-8"):
        fail("finalize-slice does not run formatting and Bootlin policy/runtime checks")
    if "tools/lql_lua_runner.c" not in MAKEFILE.read_text(encoding="utf-8"):
        fail("format command omits the Lua runtime runner")
    for path in (
        "Makefile",
        "scripts/run_lua_tests.sh",
        "scripts/check_lua_rock.sh",
        "scripts/check_lua_cli_smoke.sh",
        "scripts/check_lua_clql_parity.py",
        "scripts/check_lua_cli_parity.py",
    ):
        if "LD_LIBRARY_PATH" in (ROOT / path).read_text(encoding="utf-8"):
            fail(f"Lua local execution must not export LD_LIBRARY_PATH: {path}")
    if "LD_LIBRARY_PATH" in (ROOT / "scripts/check_pkgconfig_multiarch.sh").read_text(encoding="utf-8"):
        fail("pkg-config verification must use an ELF runtime, not LD_LIBRARY_PATH")

    fuzz_vars = cache_vars(configure["fuzz"])
    if fuzz_vars.get("CMAKE_TOOLCHAIN_FILE") != "${sourceDir}/cmake/cpkt-aflpp-toolchain.cmake":
        fail("fuzz preset does not use cmake/cpkt-aflpp-toolchain.cmake")
    if fuzz_vars.get("LQL_BUILD_FUZZERS") != "ON":
        fail("fuzz preset does not enable LQL_BUILD_FUZZERS")

    lua_vars = cache_vars(configure["debug-lua"])
    if lua_vars.get("LQL_BUILD_LUA") != "ON":
        fail("debug-lua preset does not enable LQL_BUILD_LUA")

    for preset_name, target_id in LINUX_RELEASE_TARGETS.items():
        release_vars = cache_vars(configure[preset_name])
        if release_vars.get("LQL_TARGET_ID") != target_id:
            fail(f"{preset_name} does not set LQL_TARGET_ID={target_id}")
        if release_vars.get("LQL_DIST_DIR") != "${sourceDir}/dist":
            fail(f"{preset_name} does not set LQL_DIST_DIR=${{sourceDir}}/dist")

    darwin_vars = cache_vars(configure["arm64-apple-darwin-release"])
    if darwin_vars.get("LQL_TARGET_ID") != "arm64-apple-darwin":
        fail("arm64 Darwin release preset does not set LQL_TARGET_ID=arm64-apple-darwin")
    if darwin_vars.get("LQL_DIST_DIR") != "${sourceDir}/dist":
        fail("arm64 Darwin release preset does not set LQL_DIST_DIR=${sourceDir}/dist")
    if darwin_vars.get("LQL_BUILD_DIRECT_PROBE") != "OFF":
        fail("arm64 Darwin release preset does not disable the host-only direct probe")

    print("lifecycle preset check passed")


if __name__ == "__main__":
    main()
