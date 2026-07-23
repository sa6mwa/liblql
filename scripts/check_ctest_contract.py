#!/usr/bin/env python3
import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
CMAKE = ROOT / "CMakeLists.txt"
ALLOWED_LABELS = {
    "unit",
    "smoke",
    "local",
    "packaging",
    "fuzz",
    "integration",
    "example-smoke",
    "offline",
}


def fail(message: str) -> None:
    print(f"ctest contract: {message}", file=sys.stderr)
    sys.exit(1)


def main() -> None:
    text = CMAKE.read_text(encoding="utf-8")
    tests = re.findall(r"add_test\s*\(\s*NAME\s+([A-Za-z0-9_.-]+)", text)
    contracts = {
        name: (labels, int(timeout))
        for name, labels, timeout in re.findall(
            r"lql_set_test_contract\s*\(\s*([A-Za-z0-9_.-]+)\s+"
            r"\"([A-Za-z0-9_;.-]+)\"\s+([0-9]+)\s*\)",
            text,
            re.MULTILINE,
        )
    }
    if not tests:
        fail("no add_test registrations found")
    for name in tests:
        if name not in contracts:
            fail(f"{name} is missing lql_set_test_contract")
        labels, timeout = contracts[name]
        parts = [part for part in labels.split(";") if part]
        if not parts:
            fail(f"{name} has no labels")
        unknown = [part for part in parts if part not in ALLOWED_LABELS]
        if unknown:
            fail(f"{name} has unknown labels: {', '.join(unknown)}")
        if "local" not in parts:
            fail(f"{name} must be labeled local")
        if timeout <= 0 or timeout > 60:
            fail(f"{name} timeout must be in the short local range")
    extra = sorted(set(contracts) - set(tests))
    if extra:
        fail(f"contract exists for missing tests: {', '.join(extra)}")
    print("ctest contract passed")


if __name__ == "__main__":
    main()
