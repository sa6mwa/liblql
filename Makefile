.PHONY: help deps toolchain-check lifecycle-check target-tool-check build build-debug build-release build-debug-lua test lua-test valgrind fuzz-smoke fuzz install-smoke clql-smoke package package-verify release-matrix release-pipeline prerelease release test-all direct-reset direct-no-lonejson direct-probe direct-bench direct-callback-whitespace direct-live-heap scanner-parity-smoke scanner-parity-matrix scanner-profile-hotspots format clean

help:
	@printf '%s\n' \
	  'make build-debug   build the debug self-contained scanner foundation' \
	  'make build-release build the optimized self-contained scanner foundation' \
	  'make build-debug-lua build the Lua 5.5 facade module' \
	  'make deps          ensure cached Bootlin GCC and AFL++ lifecycle tools' \
	  'make toolchain-check  run resolver syntax and unit checks' \
	  'make lifecycle-check  verify lifecycle preset/command contract' \
	  'make target-tool-check  verify target inspection tool discovery' \
	  'make build        build the debug lifecycle preset' \
	  'make test          build and run the direct-stream test suite' \
	  'make lua-test      build and run Lua 5.5 facade smoke tests' \
	  'make valgrind      run native Valgrind Memcheck gate' \
	  'make fuzz-smoke    build AFL++ target and verify instrumentation' \
	  'make fuzz          run the standard bounded AFL++ smoke gate' \
	  'make install-smoke install SDK and build CMake/pkg-config consumers' \
	  'make clql-smoke    build clql and run CLI smoke tests' \
	  'make package       build host binary SDK and checksum manifest' \
	  'make package-verify verify host binary SDK package' \
	  'make release-matrix build and verify all available SDK target packages' \
	  'make prerelease    run the full release proof graph without cleaning first' \
	  'make release       clean, then run the full release proof graph' \
	  'make test-all      run reset, dependency, test, memcheck, parity, and heap gates' \
	  'make direct-reset  verify removed execution architecture stays removed' \
	  'make direct-no-lonejson  verify liblql has no LoneJSON runtime dependency' \
	  'make direct-probe  build the optimized direct-execution probe' \
	  'make direct-bench  build the optimized direct benchmark runner' \
	  'make direct-callback-whitespace  verify whitespace callback capture uses compact spool' \
	  'make direct-live-heap  run Massif live-heap gate for direct execution' \
	  'make scanner-parity-smoke  run GCC C-vs-Go scanner parity smoke' \
	  'make scanner-parity-matrix  run broader GCC C-vs-Go scanner parity matrix' \
	  'make scanner-profile-hotspots  profile tight GCC scanner rows with perf' \
	  'make format        format retained C sources' \
	  'make clean         remove generated build output'

deps:
	@bash scripts/cpkt-toolchains.sh ensure all
	@bash scripts/cpkt-aflpp.sh ensure

toolchain-check:
	@bash -n scripts/cpkt-toolchains.sh
	@bash -n scripts/cpkt-aflpp.sh
	@bash -n scripts/test-cpkt-toolchain-resolvers.sh
	@bash -n scripts/test-cpkt-aflpp-resolver.sh
	@bash scripts/test-cpkt-toolchain-resolvers.sh
	@bash scripts/test-cpkt-aflpp-resolver.sh

lifecycle-check: toolchain-check
	@python3 scripts/check_lifecycle_presets.py
	@sh scripts/check_release_targets.sh

target-tool-check:
	@python3 -m py_compile scripts/discover_target_tools.py
	@sh scripts/test_discover_target_tools.sh

build: build-debug

build-debug:
	@cmake --preset debug
	@cmake --build --preset debug

build-release:
	@cmake --preset release
	@cmake --build --preset release

build-debug-lua:
	@cmake --preset debug-lua
	@cmake --build --preset debug-lua --target lql_lua_core

test:
	@cmake --preset debug
	@cmake --build --preset debug
	@ctest --preset debug

lua-test: build-debug-lua
	@sh scripts/run_lua_tests.sh

valgrind: build-debug
	@sh scripts/check_valgrind.sh

fuzz-smoke:
	@cmake --preset fuzz
	@cmake --build --preset fuzz --target lql_json_fuzz
	@LQL_JSON_FUZZ_PATH=build/fuzz/lql_json_fuzz sh scripts/check_fuzz_smoke.sh

fuzz: fuzz-smoke

install-smoke: build-release
	@cmake --install build/release --prefix build/install-smoke
	@sh scripts/check_install_tree.sh

clql-smoke: build-release
	@sh scripts/check_clql_smoke.sh

package:
	@sh scripts/package.sh x86_64-linux-gnu

package-verify: package
	@sh scripts/package_verify.sh x86_64-linux-gnu

release-matrix:
	@sh scripts/package_matrix.sh

release-pipeline: test-all release-matrix

prerelease: release-pipeline

release:
	@$(MAKE) clean
	@$(MAKE) release-pipeline

test-all: lifecycle-check target-tool-check direct-reset direct-no-lonejson test lua-test valgrind fuzz-smoke install-smoke clql-smoke package-verify scanner-parity-matrix direct-callback-whitespace direct-live-heap

direct-reset:
	@sh scripts/check_direct_execution_reset.sh

direct-no-lonejson: build-debug build-release
	@sh scripts/check_no_lonejson_dependency.sh

direct-probe:
	@cmake --preset release
	@cmake --build --preset release --target lql_direct_probe

direct-bench:
	@cmake --preset release
	@cmake --build --preset release --target lql_direct_bench

direct-callback-whitespace: direct-bench
	@LQL_DIRECT_BENCH_PATH=build/release/lql_direct_bench sh scripts/check_direct_callback_whitespace.sh

direct-live-heap: direct-bench
	@LQL_DIRECT_BENCH_PATH=build/release/lql_direct_bench sh scripts/check_direct_live_heap.sh

scanner-parity-smoke: direct-bench
	@mkdir -p build
	@cd reference/go-benchmark && go build -o ../../build/reference-lqlbench ./cmd/lqlbench
	@cd reference/go-benchmark && go build -o ../../build/reference-benchvalidate ./cmd/benchvalidate
	@LQL_DIRECT_BENCH_PATH=build/release/lql_direct_bench LQL_GO_BENCH_PATH=build/reference-lqlbench LQL_BENCHVALIDATE_PATH=build/reference-benchvalidate sh scripts/check_scanner_parity_smoke.sh

scanner-parity-matrix: direct-bench
	@mkdir -p build
	@cd reference/go-benchmark && go build -o ../../build/reference-lqlbench ./cmd/lqlbench
	@cd reference/go-benchmark && go build -o ../../build/reference-benchvalidate ./cmd/benchvalidate
	@LQL_DIRECT_BENCH_PATH=build/release/lql_direct_bench LQL_GO_BENCH_PATH=build/reference-lqlbench LQL_BENCHVALIDATE_PATH=build/reference-benchvalidate sh scripts/check_scanner_parity_matrix.sh

scanner-profile-hotspots: direct-bench
	@LQL_DIRECT_BENCH_PATH=build/release/lql_direct_bench sh scripts/profile_scanner_hotspots.sh

format:
	@clang-format -i include/lql/*.h src/*.c src/*.h tests/header_smoke.c tests/header_smoke.cpp tools/lql_direct_bench.c tools/clql.c fuzz/json_fuzz.c lua/lql_core.c

clean:
	@./scripts/clean.sh
