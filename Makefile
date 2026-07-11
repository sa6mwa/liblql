PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
INSTALL ?= install

.PHONY: help deps-debug deps-release deps-cross build build-clql-static build-debug build-debug-vendored-lonejson build-debug-lua build-release build-bench-release build-bench-vendored-lonejson install test test-debug test-vendored-lonejson parity-test test-all asan fuzz fuzz-smoke lua-rock lua-env lua-test bench benchmarks bench-check bench-check-vendored-lonejson bench-gate perf-gate bench-lockd-perf-check bench-memory-check bench-large-json-check bench-freeze-baseline benchmarks-go benchmarks-c benchmarks-lua benchmarks-parity package package-source package-source-smoke package-checksums package-verify verify-release-archives verify-release-privacy release-lua-artifacts release-matrix finalize-slice prerelease prerelease-hardening release print-release-version print-release-assets format clean clean-dist

help:
	@printf '%s\n' \
	  'make deps-debug              fetch host lonejson SDK' \
	  'make deps-release            fetch release lonejson SDK' \
	  'make deps-cross              fetch all release lonejson SDKs' \
	  'make build                   build static clql, preferring musl then GNU' \
	  'make build-debug             configure and build debug preset' \
	  'make build-debug-vendored-lonejson configure and build debug preset with vendored lonejson' \
	  'make build-debug-lua         configure and build debug Lua preset' \
	  'make build-release           configure and build host GNU release preset' \
	  'make build-bench-release     configure and build optimized benchmark helpers' \
	  'make build-bench-vendored-lonejson configure and build optimized helpers with vendored lonejson' \
	  'make install                 install built clql to $${PREFIX:-/usr/local}/bin' \
	  'make test                    run fast C/API tests' \
	  'make test-debug              alias for fast C/API tests' \
	  'make test-vendored-lonejson run fast C/API tests with vendored lonejson' \
	  'make parity-test             run Go-backed parity tests' \
	  'make test-all                run tests, fuzz smoke, sanitizers, and Lua smoke tests' \
	  'make asan                    run ASan/UBSan tests' \
	  'make fuzz                    alias for bounded public API fuzz smoke seeds' \
	  'make fuzz-smoke              run bounded public API fuzz smoke seeds' \
	  'make lua-rock                install Lua facade into build/luarocks' \
	  'make lua-env                 print Lua facade environment exports' \
	  'make lua-test                run Lua facade smoke tests' \
	  'make benchmarks             run local parity benchmark smoke' \
	  'make bench-check            run deterministic benchmark smoke gate' \
	  'make bench-check-vendored-lonejson run deterministic benchmark smoke gate with vendored lonejson' \
	  'make bench-gate             alias for deterministic benchmark smoke gate' \
	  'make perf-gate              alias for deterministic benchmark smoke gate' \
	  'make bench-lockd-perf-check run lockd-specific C performance gates' \
	  'make bench-memory-check     run scalable streaming and C mutation memory gates' \
	  'make bench-large-json-check run 100 MiB large-JSON memory gate' \
	  'make bench-freeze-baseline  wait for quiet host and update committed benchmark baselines' \
	  'make benchmarks-go          run Go benchmark implementation' \
	  'make benchmarks-c           run C benchmark implementation' \
	  'make benchmarks-lua         run Lua benchmark implementation' \
	  'make benchmarks-parity      require Go/C/Lua benchmark implementations' \
	  'make format                  clang-format project C sources' \
	  'make package                 build host package artifacts' \
	  'make package-source          build source archive' \
	  'make package-source-smoke    build and verify source archive' \
	  'make package-checksums       write checksum manifest' \
	  'make release-lua-artifacts   build Lua source, rockspec, and source rock' \
	  'make package-verify          verify generated packages' \
	  'make verify-release-archives verify checksum-listed release archives' \
	  'make verify-release-privacy  verify release privacy and relocatability' \
	  'make finalize-slice          format and run fast tests' \
	  'make prerelease              deterministic local prerelease gate' \
	  'make prerelease-hardening    expensive prerelease gate plus release matrix' \
	  'make release-matrix          build release target matrix where toolchains exist' \
	  'make release                 clean, test, benchmark, package, and verify release artifacts' \
	  'make print-release-version   print resolved release version' \
	  'make print-release-assets    print checksum-listed release upload assets' \
	  'make clean                   remove generated build/dist/cache/Lua state' \
	  'make clean-dist              remove generated dist artifacts'

deps-debug:
	@./scripts/deps.sh x86_64-linux-gnu

deps-release:
	@./scripts/deps.sh x86_64-linux-gnu

deps-cross:
	@./scripts/deps.sh all

build: build-clql-static

build-clql-static:
	@./scripts/build_clql_static.sh

install:
	@[ -x build/clql-static/clql ] || { \
	  printf '%s\n' 'make install requires build/clql-static/clql; run make build first' >&2; \
	  exit 1; \
	}
	@$(INSTALL) -d "$(DESTDIR)$(BINDIR)"
	@$(INSTALL) -m 0755 build/clql-static/clql "$(DESTDIR)$(BINDIR)/clql"
	@printf 'installed %s\n' "$(DESTDIR)$(BINDIR)/clql"

build-debug: deps-debug
	@cmake --preset debug
	@cmake --build --preset debug

build-debug-vendored-lonejson:
	@cmake --preset debug-vendored-lonejson
	@cmake --build --preset debug-vendored-lonejson

build-debug-lua: deps-debug
	@cmake --preset debug-lua
	@cmake --build --preset debug-lua

build-release: deps-release
	@cmake --preset x86_64-linux-gnu-release
	@cmake --build --preset x86_64-linux-gnu-release

build-bench-release: deps-release
	@cmake --preset bench-release
	@cmake --build --preset bench-release

build-bench-vendored-lonejson:
	@cmake --preset bench-vendored-lonejson
	@cmake --build --preset bench-vendored-lonejson

test test-debug: build-debug
	@ctest --preset debug -LE 'parity|fuzz'

test-vendored-lonejson: build-debug-vendored-lonejson
	@ctest --preset debug-vendored-lonejson -LE 'parity|fuzz'

parity-test: build-debug
	@ctest --preset debug -L parity

test-all: test parity-test asan fuzz-smoke lua-test

asan: deps-debug
	@cmake --preset asan
	@cmake --build --preset asan
	@ctest --preset asan -LE parity

fuzz-smoke: build-debug
	@ctest --preset debug -L fuzz

fuzz: fuzz-smoke

lua-test: build-debug-lua
	@./scripts/run_lua_tests.sh

lua-rock:
	@./scripts/build_lua_rock.sh

lua-env:
	@printf 'export LUA_PATH=%s/build/luarocks/share/lua/5.5/?.lua;%s/build/luarocks/share/lua/5.5/?/init.lua;%s/lua/?.lua;%s/lua/?/init.lua;;\n' "$$(pwd)" "$$(pwd)" "$$(pwd)" "$$(pwd)"
	@printf 'export LUA_CPATH=%s/build/luarocks/lib/lua/5.5/?.so;%s/build/luarocks/lib/lua/5.5/?/core.so;%s/build/debug-lua/?.so;%s/build/debug-lua/?/core.so;;\n' "$$(pwd)" "$$(pwd)" "$$(pwd)" "$$(pwd)"
	@printf 'export LD_LIBRARY_PATH=%s/build/debug-lua:%s/.cache/deps/x86_64-linux-gnu/install/lib:$${LD_LIBRARY_PATH:-}\n' "$$(pwd)" "$$(pwd)"
	@printf 'export DYLD_LIBRARY_PATH=%s/build/debug-lua:%s/.cache/deps/x86_64-linux-gnu/install/lib:$${DYLD_LIBRARY_PATH:-}\n' "$$(pwd)" "$$(pwd)"

bench benchmarks: build-debug
	@./scripts/check_parity_benchmark_schema.sh

bench-check: build-debug build-debug-lua build-bench-release
	@mkdir -p build
	@LQL_PAYLOAD_BENCH_PATH=build/bench-release/lql_payload_bench \
	  LQL_BENCH_LIBRARY_DIR=build/bench-release \
	  LQL_BENCH_SUITE=smoke \
	  ./scripts/run_parity_benchmarks.sh --impl go,c,lua --format json --check --require go,c,lua > build/bench-check.jsonl
	@LQL_PAYLOAD_BENCH_PATH=build/bench-release/lql_payload_bench \
	  LQL_BENCH_LIBRARY_DIR=build/bench-release \
	  ./scripts/check_lockd_perf_benchmark.sh
	@./scripts/check_parity_benchmark_failures.sh
	@./scripts/check_parity_benchmark_fixtures.sh
	@./scripts/check_parity_benchmark_memory.sh build/bench-check.jsonl
	@(cd parity && "$${GO:-go}" run ./cmd/benchvalidate --min-c-go-speedup=1.2) < build/bench-check.jsonl
	@./scripts/check_parity_benchmark_schema.sh

bench-check-vendored-lonejson: build-debug-vendored-lonejson build-debug-lua build-bench-vendored-lonejson
	@mkdir -p build
	@LQL_PAYLOAD_BENCH_PATH=build/bench-vendored-lonejson/lql_payload_bench \
	  LQL_BENCH_LIBRARY_DIR=build/bench-vendored-lonejson \
	  LQL_BENCH_SUITE=smoke \
	  ./scripts/run_parity_benchmarks.sh --impl go,c,lua --format json --check --require go,c,lua > build/bench-check-vendored-lonejson.jsonl
	@LQL_PAYLOAD_BENCH_PATH=build/bench-vendored-lonejson/lql_payload_bench \
	  LQL_BENCH_LIBRARY_DIR=build/bench-vendored-lonejson \
	  ./scripts/check_lockd_perf_benchmark.sh
	@LQL_PAYLOAD_BENCH_PATH=build/bench-vendored-lonejson/lql_payload_bench \
	  LQL_BENCH_LIBRARY_DIR=build/bench-vendored-lonejson \
	  ./scripts/check_parity_benchmark_failures.sh
	@./scripts/check_parity_benchmark_fixtures.sh
	@./scripts/check_parity_benchmark_memory.sh build/bench-check-vendored-lonejson.jsonl
	@(cd parity && "$${GO:-go}" run ./cmd/benchvalidate --min-c-go-speedup=1.2) < build/bench-check-vendored-lonejson.jsonl
	@LQL_PAYLOAD_BENCH_PATH=build/bench-vendored-lonejson/lql_payload_bench \
	  LQL_BENCH_LIBRARY_DIR=build/bench-vendored-lonejson \
	  ./scripts/check_parity_benchmark_schema.sh

bench-gate perf-gate: bench-check

bench-lockd-perf-check: build-bench-release
	@LQL_PAYLOAD_BENCH_PATH=build/bench-release/lql_payload_bench \
	  LQL_BENCH_LIBRARY_DIR=build/bench-release \
	  ./scripts/check_lockd_perf_benchmark.sh

bench-memory-check: build-debug build-bench-release
	@LQL_PAYLOAD_BENCH_PATH=build/bench-release/lql_payload_bench \
	  LQL_BENCH_LIBRARY_DIR=build/bench-release \
	  ./scripts/check_parity_benchmark_large_memory.sh

bench-large-json-check: build-debug build-bench-release
	@LQL_PAYLOAD_BENCH_PATH=build/bench-release/lql_payload_bench \
	  LQL_BENCH_LIBRARY_DIR=build/bench-release \
	  ./scripts/check_parity_benchmark_large_json_memory.sh

bench-freeze-baseline: build-debug build-bench-release
	@LQL_PAYLOAD_BENCH_PATH=build/bench-release/lql_payload_bench \
	  LQL_BENCH_LIBRARY_DIR=build/bench-release \
	  ./scripts/bench_freeze_baseline.sh

benchmarks-go:
	@./scripts/run_parity_benchmarks.sh --impl go --format json

benchmarks-c: build-debug
	@./scripts/run_parity_benchmarks.sh --impl c --format json

benchmarks-lua: build-debug-lua
	@./scripts/run_parity_benchmarks.sh --impl lua --format json

benchmarks-parity: build-debug build-debug-lua
	@mkdir -p build
	@LQL_BENCH_SUITE=smoke ./scripts/run_parity_benchmarks.sh --impl go,c,lua --format json --check --require go,c,lua > build/benchmarks-parity.jsonl
	@(cd parity && "$${GO:-go}" run ./cmd/benchvalidate --forbid-unsupported) < build/benchmarks-parity.jsonl

package package-source package-source-smoke package-checksums package-verify verify-release-archives verify-release-privacy release-lua-artifacts release-matrix:
	@./scripts/package.sh $@

finalize-slice: format test

prerelease: format test-all bench-check package-verify

prerelease-hardening: prerelease bench-large-json-check release-matrix

release:
	@./scripts/release_gate.sh

print-release-version:
	@./scripts/release_version.sh

print-release-assets:
	@./scripts/package.sh print-release-assets

format:
	@clang-format -i include/lql/*.h src/*.c src/*.h tests/*.c examples/*.c bench/*.c lua/*.c

clean:
	@./scripts/clean.sh

clean-dist:
	@./scripts/clean.sh --dist
