.PHONY: help deps deps-debug deps-release deps-cross toolchain-check lifecycle-check target-tool-check build build-debug build-release build-debug-lua test test-debug lua-test lua-rock lua-cli-smoke lua-env release-lua-artifacts lua-artifact-smoke lua-artifact-privacy-regression valgrind fuzz-smoke fuzz install-smoke clql-smoke package package-source package-source-smoke package-checksums package-verify verify-release-archives verify-release-privacy release-matrix release-pipeline prerelease prerelease-hardening prerelease-live release test-all direct-reset direct-no-lonejson direct-probe direct-bench direct-callback-whitespace direct-live-heap direct-parity-smoke direct-parity-matrix direct-profile-hotspots bench-gate perf-gate finalize-slice format clean clean-dist

help:
	@printf '%s\n' \
	  'make deps-debug    ensure native debug Bootlin GCC lifecycle tools' \
	  'make deps-release  ensure all release Bootlin GCC lifecycle tools' \
	  'make deps-cross    ensure all cross Bootlin GCC lifecycle tools' \
	  'make deps          ensure cached Bootlin GCC and AFL++ lifecycle tools' \
	  'make toolchain-check  run resolver syntax and unit checks' \
	  'make lifecycle-check  verify lifecycle preset/command contract' \
	  'make target-tool-check  verify target inspection tool discovery' \
	  'make build        build the debug lifecycle preset' \
	  'make build-debug   build the debug self-contained liblql direct-execution implementation' \
	  'make build-release build the optimized self-contained liblql direct-execution implementation' \
	  'make build-debug-lua build the Lua 5.5 facade module' \
	  'make test          build and run the direct-stream test suite' \
	  'make test-debug    alias for make test' \
	  'make lua-test      build and run Lua 5.5 facade smoke tests' \
	  'make lua-rock      install Lua facade into repo-local LuaRocks tree' \
	  'make lua-cli-smoke run installed lql.lua CLI smoke tests' \
	  'make lua-env       print environment for repo-local LuaRocks tree' \
	  'make lua-artifact-smoke verify Lua release artifacts' \
	  'make lua-artifact-privacy-regression verify Lua artifact leak fixtures fail closed' \
	  'make valgrind      run native Valgrind Memcheck gate' \
	  'make fuzz-smoke    build AFL++ target and verify instrumentation' \
	  'make fuzz          run the standard bounded AFL++ smoke gate' \
	  'make install-smoke install SDK and build CMake/pkg-config consumers' \
	  'make clql-smoke    build clql and run CLI smoke tests' \
	  'make package       build host binary SDK and checksum manifest' \
	  'make package-source build source archive and append checksum manifest' \
	  'make package-source-smoke verify source archive from checksum manifest' \
	  'make package-checksums verify the release checksum manifest' \
	  'make package-verify verify host binary SDK package' \
	  'make verify-release-archives verify checksum-listed release archives' \
	  'make verify-release-privacy verify release artifact privacy/relocatability' \
	  'make release-matrix build and verify all available SDK target packages' \
	  'make prerelease    run the full release proof graph without cleaning first' \
	  'make prerelease-hardening alias for prerelease until extra hardening exists' \
	  'make prerelease-live fail-closed placeholder for opt-in live checks' \
	  'make release       clean, then run the full release proof graph' \
	  'make test-all      run reset, dependency, test, memcheck, parity, and heap gates' \
	  'make direct-reset  verify removed execution architecture stays removed' \
	  'make direct-no-lonejson  verify liblql has no LoneJSON runtime dependency' \
	  'make direct-probe  build the optimized direct-execution probe' \
	  'make direct-bench  build the optimized direct benchmark runner' \
	  'make direct-callback-whitespace  verify whitespace callback capture uses compact spool' \
	  'make direct-live-heap  run Massif live-heap gate for direct execution' \
	  'make direct-parity-smoke  run GCC C-vs-Go direct-execution parity smoke' \
	  'make direct-parity-matrix run broader GCC C-vs-Go direct-execution parity matrix' \
	  'make direct-profile-hotspots profile tight GCC direct-execution rows with perf' \
	  'make bench-gate    run the accepted direct-execution performance gate' \
	  'make perf-gate     alias for bench-gate' \
	  'make finalize-slice run formatting and debug tests for a small slice' \
	  'make format        format retained C sources' \
	  'make clean-dist    remove generated release artifacts under dist/' \
	  'make clean         remove generated build and release output'

deps:
	@bash scripts/cpkt-toolchains.sh ensure all
	@bash scripts/cpkt-aflpp.sh ensure

deps-debug:
	@bash scripts/cpkt-toolchains.sh ensure x86_64-linux-gnu

deps-release:
	@bash scripts/cpkt-toolchains.sh ensure all
	@bash scripts/cpkt-aflpp.sh ensure

deps-cross:
	@bash scripts/cpkt-toolchains.sh ensure all

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
	@cmake --preset release
	@cmake --build --preset release
	@rm -rf build/lua-sdk
	@cmake --install build/release --prefix build/lua-sdk
	@cmake --preset debug-lua
	@cmake --build --preset debug-lua --target lql_lua_core

test:
	@cmake --preset debug
	@cmake --build --preset debug
	@ctest --preset debug

test-debug: test

lua-test: build-debug-lua
	@sh scripts/run_lua_tests.sh

lua-rock:
	@sh scripts/build_lua_rock.sh
	@sh scripts/check_lua_rock.sh

lua-cli-smoke: lua-rock
	@sh scripts/check_lua_cli_smoke.sh

lua-env:
	@printf 'export LUA_PATH=%s/share/lua/5.5/?.lua;%s/share/lua/5.5/?/init.lua;;\n' "$$(pwd -P)/build/luarocks" "$$(pwd -P)/build/luarocks"
	@printf 'export LUA_CPATH=%s/lib/lua/5.5/?.so;%s/lib/lua/5.5/?/core.so;;\n' "$$(pwd -P)/build/luarocks" "$$(pwd -P)/build/luarocks"
	@printf 'export LD_LIBRARY_PATH=%s/lib:$${LD_LIBRARY_PATH:-}\n' "$$(pwd -P)/build/lua-sdk"

release-lua-artifacts:
	@sh scripts/package_lua.sh

lua-artifact-smoke: release-lua-artifacts
	@sh scripts/package-verify.sh lua

lua-artifact-privacy-regression:
	@sh scripts/check_lua_artifact_privacy_regression.sh

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

package-source:
	@sh scripts/package_source.sh

package-source-smoke: package-source
	@sh scripts/package-verify.sh source

package-checksums:
	@sh scripts/package-verify.sh checksums

package-verify: package
	@sh scripts/package-verify.sh x86_64-linux-gnu

verify-release-archives:
	@sh scripts/package-verify.sh archives

verify-release-privacy:
	@sh scripts/package-verify.sh privacy

release-matrix:
	@sh scripts/package_matrix.sh

release-pipeline: test-all release-matrix

prerelease: release-pipeline

prerelease-hardening: prerelease

prerelease-live:
	@printf '%s\n' 'SKIP: liblql has no live external-provider prerelease checks'

release:
	@$(MAKE) clean
	@$(MAKE) release-pipeline

test-all: lifecycle-check target-tool-check direct-reset direct-no-lonejson test lua-test lua-cli-smoke lua-artifact-privacy-regression valgrind fuzz-smoke install-smoke clql-smoke package-verify direct-parity-matrix direct-callback-whitespace direct-live-heap

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

direct-parity-smoke: direct-bench
	@mkdir -p build
	@cd reference/go-benchmark && go build -o ../../build/reference-lqlbench ./cmd/lqlbench
	@cd reference/go-benchmark && go build -o ../../build/reference-benchvalidate ./cmd/benchvalidate
	@LQL_DIRECT_BENCH_PATH=build/release/lql_direct_bench LQL_GO_BENCH_PATH=build/reference-lqlbench LQL_BENCHVALIDATE_PATH=build/reference-benchvalidate sh scripts/check_direct_parity_smoke.sh

direct-parity-matrix: direct-bench
	@mkdir -p build
	@cd reference/go-benchmark && go build -o ../../build/reference-lqlbench ./cmd/lqlbench
	@cd reference/go-benchmark && go build -o ../../build/reference-benchvalidate ./cmd/benchvalidate
	@LQL_DIRECT_BENCH_PATH=build/release/lql_direct_bench LQL_GO_BENCH_PATH=build/reference-lqlbench LQL_BENCHVALIDATE_PATH=build/reference-benchvalidate sh scripts/check_direct_parity_matrix.sh

direct-profile-hotspots: direct-bench
	@LQL_DIRECT_BENCH_PATH=build/release/lql_direct_bench sh scripts/profile_direct_hotspots.sh

bench-gate: direct-parity-matrix

perf-gate: bench-gate

finalize-slice: format test-debug

format:
	@clang-format -i include/lql/*.h src/*.c src/*.h tests/header_smoke.c tests/header_smoke.cpp tools/lql_direct_bench.c tools/clql.c fuzz/json_fuzz.c lua/lql_core.c

clean-dist:
	@./scripts/clean.sh dist

clean:
	@./scripts/clean.sh
