.PHONY: help deps-debug deps-release deps-cross build build-debug build-release test test-debug parity-test test-all asan lua-rock lua-env lua-test bench benchmarks bench-check bench-memory-check bench-1g-check benchmarks-go benchmarks-c benchmarks-lua benchmarks-parity package package-source package-source-smoke package-checksums package-verify verify-release-archives verify-release-privacy release-lua-artifacts release-matrix finalize-slice prerelease prerelease-hardening release print-release-version format clean clean-dist

help:
	@printf '%s\n' \
	  'make deps-debug              fetch host lonejson SDK' \
	  'make build                   configure and build debug preset' \
	  'make test                    run fast C/API tests' \
	  'make parity-test             run Go-backed parity tests' \
	  'make test-all                run tests, sanitizers, and Lua smoke tests' \
	  'make asan                    run ASan/UBSan tests' \
	  'make lua-rock                install Lua facade into build/luarocks' \
	  'make lua-env                 print Lua facade environment exports' \
	  'make lua-test                run Lua facade smoke tests' \
	  'make benchmarks             run local parity benchmark smoke' \
	  'make bench-check            run deterministic benchmark smoke gate' \
	  'make bench-memory-check     run scalable Go/C/Lua streaming memory gate' \
	  'make bench-1g-check         run 1 GiB/128 MiB streaming memory gate' \
	  'make benchmarks-parity      require Go/C/Lua benchmark implementations' \
	  'make format                  clang-format project C sources' \
	  'make package                 build host package artifacts' \
	  'make release-lua-artifacts   build Lua source, rockspec, and source rock' \
	  'make package-verify          verify generated packages' \
	  'make release-matrix          build release target matrix where toolchains exist' \
	  'make release                 clean, test, benchmark, package, and verify release artifacts' \
	  'make clean                   remove generated build/dist/cache state'

deps-debug:
	@./scripts/deps.sh x86_64-linux-gnu

deps-release:
	@./scripts/deps.sh x86_64-linux-gnu

deps-cross:
	@./scripts/deps.sh all

build build-debug: deps-debug
	@cmake --preset debug
	@cmake --build --preset debug

build-release: deps-release
	@cmake --preset x86_64-linux-gnu-release
	@cmake --build --preset x86_64-linux-gnu-release

test test-debug: build-debug
	@ctest --preset debug -LE parity

parity-test: build-debug
	@ctest --preset debug -L parity

test-all: test parity-test asan lua-test

asan: deps-debug
	@cmake --preset asan
	@cmake --build --preset asan
	@ctest --preset asan -LE parity

lua-test: build-debug
	@./scripts/run_lua_tests.sh

lua-rock:
	@./scripts/build_lua_rock.sh

lua-env:
	@printf 'export LUA_PATH=%s/build/luarocks/share/lua/5.5/?.lua;%s/build/luarocks/share/lua/5.5/?/init.lua;%s/lua/?.lua;%s/lua/?/init.lua;;\n' "$$(pwd)" "$$(pwd)" "$$(pwd)" "$$(pwd)"
	@printf 'export LUA_CPATH=%s/build/luarocks/lib/lua/5.5/?.so;%s/build/luarocks/lib/lua/5.5/?/core.so;%s/build/debug/?.so;%s/build/debug/?/core.so;;\n' "$$(pwd)" "$$(pwd)" "$$(pwd)" "$$(pwd)"
	@printf 'export LD_LIBRARY_PATH=%s/build/debug:%s/.cache/deps/x86_64-linux-gnu/install/lib:$${LD_LIBRARY_PATH:-}\n' "$$(pwd)" "$$(pwd)"
	@printf 'export DYLD_LIBRARY_PATH=%s/build/debug:%s/.cache/deps/x86_64-linux-gnu/install/lib:$${DYLD_LIBRARY_PATH:-}\n' "$$(pwd)" "$$(pwd)"

bench benchmarks: build-debug
	@./scripts/check_parity_benchmark_schema.sh

bench-check: build-debug
	@mkdir -p build
	@LQL_BENCH_SUITE=smoke ./scripts/run_parity_benchmarks.sh --impl go,c,lua --format json --check --require go,c,lua > build/bench-check.jsonl
	@./scripts/check_parity_benchmark_failures.sh
	@./scripts/check_parity_benchmark_fixtures.sh
	@./scripts/check_parity_benchmark_memory.sh build/bench-check.jsonl
	@./scripts/check_parity_benchmark_schema.sh

bench-memory-check: build-debug
	@./scripts/check_parity_benchmark_large_memory.sh

bench-1g-check: build-debug
	@./scripts/check_parity_benchmark_1g_memory.sh

benchmarks-go:
	@./scripts/run_parity_benchmarks.sh --impl go --format json

benchmarks-c: build-debug
	@./scripts/run_parity_benchmarks.sh --impl c --format json

benchmarks-lua:
	@./scripts/run_parity_benchmarks.sh --impl lua --format json

benchmarks-parity: build-debug
	@./scripts/run_parity_benchmarks.sh --impl go,c,lua --format json --require go,c,lua

package package-source package-source-smoke package-checksums package-verify verify-release-archives verify-release-privacy release-lua-artifacts release-matrix:
	@./scripts/package.sh $@

finalize-slice: format test

prerelease: format test-all bench-check package-verify

prerelease-hardening: prerelease bench-1g-check release-matrix

release:
	@./scripts/release_gate.sh

print-release-version:
	@./scripts/release_version.sh

format:
	@clang-format -i include/lql/*.h src/*.c src/*.h tests/*.c examples/*.c bench/*.c lua/*.c

clean:
	@./scripts/clean.sh

clean-dist:
	@rm -rf dist
