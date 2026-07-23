.PHONY: help deps deps-debug deps-release deps-cross toolchain-check dependency-cache-check dependency-cache-privacy-regression tar-portability-check lifecycle-check lifecycle-version-contract ctest-contract target-tool-check build build-debug build-release build-debug-lua test test-debug mutation-literal-parity shared-only-smoke cross-build cross-test lua-test lua-rock lua-cli-smoke lua-cli-clql-parity lua-cli-parity lua-env release-lua-artifacts lua-artifact-smoke lua-artifact-privacy-regression valgrind fuzz-smoke fuzz install-smoke clql-smoke clql-selector-parity clql-mutation-parity package package-clql package-source package-source-smoke source-manifest-exactness package-checksums package-verify release-upload-list verify-release-archives verify-release-privacy release-matrix release-pipeline prerelease prerelease-hardening prerelease-live release print-release-version test-all print-test-all-gates test-all-timed test-all-gates selector-ast-contract sdk-parity-contract direct-reset direct-no-lonejson direct-probe direct-bench direct-callback-whitespace direct-live-heap sdk-parity-gate direct-parity-matrix direct-perf-gate direct-profile-hotspots bench-gate perf-gate finalize-slice format clean clean-dist

TEST_ALL_GATES := lifecycle-check target-tool-check ctest-contract selector-ast-contract sdk-parity-contract direct-reset direct-no-lonejson test mutation-literal-parity shared-only-smoke lua-test lua-cli-smoke lua-cli-clql-parity lua-cli-parity lua-artifact-privacy-regression dependency-cache-privacy-regression valgrind fuzz-smoke install-smoke clql-smoke clql-selector-parity clql-mutation-parity package-verify sdk-parity-gate direct-perf-gate direct-callback-whitespace direct-live-heap

help:
	@printf '%s\n' \
	  'make deps-debug    ensure native debug Bootlin GCC lifecycle tools' \
	  'make deps-release  ensure all release Bootlin GCC lifecycle tools' \
	  'make deps-cross    ensure all cross Bootlin GCC lifecycle tools' \
	  'make deps          ensure cached Bootlin GCC and AFL++ lifecycle tools' \
	  'make toolchain-check  run resolver syntax and unit checks' \
	  'make dependency-cache-check  verify shared dependency-cache resolution' \
	  'make dependency-cache-privacy-regression  prove release privacy rejects dependency-cache paths' \
	  'make tar-portability-check  verify package tar option detection' \
	  'make lifecycle-check  verify lifecycle preset/command contract' \
	  'make lifecycle-version-contract  verify exact-tag and override version contract' \
	  'make ctest-contract  verify CTest labels and timeouts' \
	  'make target-tool-check  verify target inspection tool discovery' \
	  'make build        build the debug lifecycle preset' \
	  'make build-debug   build the debug self-contained liblql direct-execution implementation' \
	  'make build-release build the optimized self-contained liblql direct-execution implementation' \
	  'make build-debug-lua build the Lua 5.5 facade module' \
	  'make test          build and run the direct-stream test suite' \
	  'make test-debug    alias for make test' \
	  'make mutation-literal-parity  verify C and pinned Go mutation literal semantics' \
	  'make shared-only-smoke  configure, build, and test without liblql.a' \
	  'make cross-build   build and verify release cross-target package matrix' \
	  'make cross-test    run cross-target tool and Darwin linker-route checks' \
	  'make lua-test      build and run Lua 5.5 facade smoke tests' \
	  'make lua-rock      install Lua facade into repo-local LuaRocks tree' \
	  'make lua-cli-smoke run installed lql.lua CLI smoke tests' \
	  'make lua-cli-clql-parity compare lql.lua output against clql' \
	  'make lua-cli-parity compare lql.lua output against Go lql' \
	  'make lua-env       print environment for repo-local LuaRocks tree' \
	  'make lua-artifact-smoke verify Lua release artifacts' \
	  'make lua-artifact-privacy-regression verify Lua artifact leak fixtures fail closed' \
	  'make valgrind      run native Valgrind Memcheck gate' \
	  'make fuzz-smoke    build AFL++ target and verify instrumentation' \
	  'make fuzz          run the standard bounded AFL++ smoke gate' \
	  'make install-smoke install SDK and build CMake/pkg-config consumers, including multiarch pkg-config' \
	  'make clql-smoke    build clql and run CLI smoke tests' \
	  'make clql-selector-parity compare clql selector output against Go lql' \
	  'make clql-mutation-parity compare clql mutation output against Go lql' \
	  'make package       build host liblql SDK and checksum manifest' \
	  'make package-clql  build host static clql runtime archive and append checksum' \
	  'make package-source build source archive and append checksum manifest' \
	  'make package-source-smoke verify source archive from checksum manifest' \
	  'make source-manifest-exactness verify source archive payload matches RELEASE_MANIFEST' \
		  'make package-checksums verify the release checksum manifest' \
		  'make package-verify verify checksum-listed host package artifacts' \
		  'make release-upload-list print manifest-selected release upload files' \
		  'make verify-release-archives verify checksum-listed release archives' \
	  'make verify-release-privacy verify release artifact privacy/relocatability' \
	  'make release-matrix build and verify all available SDK target packages' \
	  'make prerelease    run the full release proof graph without cleaning first' \
	  'make prerelease-hardening alias for prerelease until extra hardening exists' \
	  'make prerelease-live fail-closed placeholder for opt-in live checks' \
	  'make release       verify version contract, clean, then run the full release proof graph' \
	  'make print-release-version print the version used by package/release targets' \
	  'make test-all      run reset, dependency, test, memcheck, parity, and heap gates with elapsed time' \
	  'make selector-ast-contract  verify selector AST oracle inventory and source contract' \
	  'make sdk-parity-contract  verify SDK parity gate coverage contract' \
	  'make direct-reset  verify removed execution architecture stays removed' \
	  'make direct-no-lonejson  verify liblql has no LoneJSON runtime dependency' \
	  'make direct-probe  build the optimized direct-execution probe' \
	  'make direct-bench  build the optimized direct benchmark runner' \
	  'make direct-callback-whitespace  verify whitespace callback capture uses compact spool' \
	  'make direct-live-heap  run Massif live-heap gate for direct execution' \
	  'make sdk-parity-gate  run fast liblql SDK-vs-Go parity gate' \
	  'make direct-parity-matrix run broader liblql SDK-vs-Go parity matrix' \
	  'make direct-perf-gate run selected hard C-vs-Go direct performance/RSS gate' \
	  'make direct-profile-hotspots profile tight GCC direct-execution rows with perf' \
	  'make bench-gate    run the accepted direct-execution performance gate' \
	  'make perf-gate     alias for bench-gate' \
	  'make finalize-slice run formatting and debug tests for a small slice' \
	  'make format        format retained C sources' \
	  'make clean-dist    remove generated release artifacts under dist/' \
	  'make clean         remove generated build and release output'

deps:
	@sh scripts/deps.sh

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

dependency-cache-check:
	@sh scripts/test_dependency_cache_config.sh

dependency-cache-privacy-regression:
	@sh scripts/check_dependency_cache_privacy_regression.sh

tar-portability-check:
	@sh scripts/check_tar_portability.sh

lifecycle-check: toolchain-check dependency-cache-check tar-portability-check ctest-contract sdk-parity-contract
	@python3 scripts/check_lifecycle_presets.py
	@sh scripts/check_release_targets.sh

lifecycle-version-contract:
	@sh scripts/check_lifecycle_version_contract.sh

ctest-contract:
	@python3 scripts/check_ctest_contract.py

target-tool-check:
	@python3 -c 'import ast,pathlib; ast.parse(pathlib.Path("scripts/discover_target_tools.py").read_text(), "scripts/discover_target_tools.py")'
	@sh scripts/test_discover_target_tools.sh
	@sh scripts/check_darwin_linker_route.sh

build: build-debug

selector-ast-contract:
	@sh scripts/check_selector_ast_contract.sh

sdk-parity-contract:
	@sh scripts/check_sdk_parity_contract.sh

build-debug:
	@sh scripts/build.sh

build-release:
	@cmake --preset release
	@cmake --build --preset release

build-debug-lua:
	@cmake --preset release
	@cmake --build --preset release
	@sh scripts/remove_path.sh build/lua-sdk
	@cmake --install build/release --prefix build/lua-sdk
	@cmake --preset debug-lua
	@cmake --build --preset debug-lua --target lql_lua_core

test:
	@sh scripts/test.sh

test-debug: test

mutation-literal-parity: test
	@cd reference/go-benchmark && go test .

shared-only-smoke:
	@sh scripts/check_shared_only_build.sh

cross-build:
	@sh scripts/cross_build.sh

cross-test:
	@sh scripts/cross_test.sh

lua-test: build-debug-lua
	@sh scripts/run_lua_tests.sh

lua-rock:
	@sh scripts/build_lua_rock.sh
	@sh scripts/check_lua_rock.sh

lua-cli-smoke: lua-rock
	@sh scripts/check_lua_cli_smoke.sh

lua-cli-clql-parity: build-release lua-rock
	@CLQL_PATH=build/release/clql LQL_LUA_CLI_PATH=build/luarocks/bin/lql.lua python3 scripts/check_lua_clql_parity.py

lua-cli-parity: lua-rock
	@mkdir -p build
	@repo=$$(pwd); modver=$$(cd reference/go-benchmark && go list -m -f '{{.Version}}' pkt.systems/lql); moddir="$$(go env GOPATH)/pkg/mod/pkt.systems/lql@$$modver"; cd "$$moddir" && go build -o "$$repo/build/reference-lql" ./cmd/lql
	@LQL_GO_CLI_PATH=build/reference-lql LQL_LUA_CLI_PATH=build/luarocks/bin/lql.lua python3 scripts/check_lua_cli_parity.py

lua-env:
	@printf "export LUA_PATH='%s/share/lua/5.5/?.lua;%s/share/lua/5.5/?/init.lua;;'\n" "$$(pwd -P)/build/luarocks" "$$(pwd -P)/build/luarocks"
	@printf "export LUA_CPATH='%s/lib/lua/5.5/?.so;%s/lib/lua/5.5/?/core.so;;'\n" "$$(pwd -P)/build/luarocks" "$$(pwd -P)/build/luarocks"
	@printf "export LD_LIBRARY_PATH='%s/lib'$${LD_LIBRARY_PATH:+\":$$LD_LIBRARY_PATH\"}\n" "$$(pwd -P)/build/lua-sdk"

release-lua-artifacts:
	@sh scripts/package_lua.sh

lua-artifact-smoke: release-lua-artifacts
	@sh scripts/package-verify.sh lua

lua-artifact-privacy-regression:
	@sh scripts/check_lua_artifact_privacy_regression.sh

valgrind: build-debug
	@sh scripts/check_valgrind.sh

fuzz-smoke:
	@sh scripts/fuzz.sh

fuzz: fuzz-smoke

install-smoke: build-release
	@cmake --install build/release --prefix build/install-smoke
	@sh scripts/check_install_tree.sh
	@sh scripts/check_pkgconfig_multiarch.sh

clql-smoke: build-release
	@mkdir -p build
	@repo=$$(pwd); modver=$$(cd reference/go-benchmark && go list -m -f '{{.Version}}' pkt.systems/lql); moddir="$$(go env GOPATH)/pkg/mod/pkt.systems/lql@$$modver"; cd "$$moddir" && go build -o "$$repo/build/reference-lql" ./cmd/lql
	@LQL_GO_CLI_PATH=build/reference-lql sh scripts/check_clql_smoke.sh

clql-selector-parity: build-release
	@mkdir -p build
	@repo=$$(pwd); modver=$$(cd reference/go-benchmark && go list -m -f '{{.Version}}' pkt.systems/lql); moddir="$$(go env GOPATH)/pkg/mod/pkt.systems/lql@$$modver"; cd "$$moddir" && go build -o "$$repo/build/reference-lql" ./cmd/lql
	@LQL_GO_CLI_PATH=build/reference-lql CLQL_PATH=build/release/clql python3 scripts/check_clql_selector_parity.py

clql-mutation-parity: build-release
	@mkdir -p build
	@repo=$$(pwd); modver=$$(cd reference/go-benchmark && go list -m -f '{{.Version}}' pkt.systems/lql); moddir="$$(go env GOPATH)/pkg/mod/pkt.systems/lql@$$modver"; cd "$$moddir" && go build -o "$$repo/build/reference-lql" ./cmd/lql
	@LQL_GO_CLI_PATH=build/reference-lql CLQL_PATH=build/release/clql python3 scripts/check_clql_mutation_parity.py

package:
	@sh scripts/package.sh x86_64-linux-gnu

package-clql: package
	@LQL_PACKAGE_CHECKSUM_MODE=append sh scripts/package_clql.sh x86_64-linux-gnu

package-source:
	@sh scripts/stage_release_sources.sh

package-source-smoke:
	@sh scripts/test_release_from_source.sh

source-manifest-exactness: package-source
	@sh scripts/check_source_manifest_exactness.sh

package-checksums:
	@sh scripts/package-verify.sh checksums

package-verify: package-clql
	@sh scripts/package-verify.sh all

release-upload-list:
	@sh scripts/print_release_uploads.sh

verify-release-archives:
	@sh scripts/verify_release_artifacts.sh

verify-release-privacy:
	@sh scripts/verify_release_privacy.sh

release-matrix:
	@sh scripts/run_linux_release_matrix.sh

release-pipeline:
	@$(MAKE) --no-print-directory -f Makefile test-all
	@$(MAKE) --no-print-directory -f Makefile direct-parity-matrix
	@$(MAKE) --no-print-directory -f Makefile release-matrix

prerelease: release-pipeline

prerelease-hardening: prerelease

prerelease-live:
	@printf '%s\n' 'SKIP: liblql has no live external-provider prerelease checks'

release:
	@$(MAKE) lifecycle-version-contract
	@$(MAKE) clean
	@$(MAKE) release-pipeline

print-release-version:
	@sh scripts/release_version.sh

test-all:
	@$(MAKE) --no-print-directory -f Makefile test-all-timed

test-all-timed:
	@sh scripts/test_all.sh

print-test-all-gates:
	@printf '%s\n' '$(TEST_ALL_GATES)'

test-all-gates:
	@TEST_ALL_GATES='$(TEST_ALL_GATES)' sh scripts/test_all.sh

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

sdk-parity-gate: direct-bench
	@mkdir -p build
	@cd reference/go-benchmark && go build -o ../../build/reference-lqlbench ./cmd/lqlbench
	@cd reference/go-benchmark && go build -o ../../build/reference-benchvalidate ./cmd/benchvalidate
	@LQL_DIRECT_BENCH_PATH=build/release/lql_direct_bench LQL_GO_BENCH_PATH=build/reference-lqlbench LQL_BENCHVALIDATE_PATH=build/reference-benchvalidate sh scripts/check_direct_parity_smoke.sh

direct-parity-matrix: direct-bench
	@mkdir -p build
	@cd reference/go-benchmark && go build -o ../../build/reference-lqlbench ./cmd/lqlbench
	@cd reference/go-benchmark && go build -o ../../build/reference-benchvalidate ./cmd/benchvalidate
	@LQL_DIRECT_BENCH_PATH=build/release/lql_direct_bench LQL_GO_BENCH_PATH=build/reference-lqlbench LQL_BENCHVALIDATE_PATH=build/reference-benchvalidate sh scripts/check_direct_parity_matrix.sh

direct-perf-gate: direct-bench
	@mkdir -p build
	@cd reference/go-benchmark && go build -o ../../build/reference-lqlbench ./cmd/lqlbench
	@cd reference/go-benchmark && go build -o ../../build/reference-benchvalidate ./cmd/benchvalidate
	@LQL_DIRECT_BENCH_PATH=build/release/lql_direct_bench LQL_GO_BENCH_PATH=build/reference-lqlbench LQL_BENCHVALIDATE_PATH=build/reference-benchvalidate sh scripts/check_direct_perf_gate.sh

direct-profile-hotspots: direct-bench
	@LQL_DIRECT_BENCH_PATH=build/release/lql_direct_bench sh scripts/profile_direct_hotspots.sh

bench-gate: direct-perf-gate

perf-gate: bench-gate

finalize-slice: format test-debug

format:
	@clang-format -i include/lql/*.h src/*.c src/*.h tools/lql_direct_bench.c tools/clql.c fuzz/json_fuzz.c lua/lql_core.c examples/*.c

clean-dist:
	@./scripts/clean.sh dist

clean:
	@./scripts/clean.sh
