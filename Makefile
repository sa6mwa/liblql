.PHONY: help build-debug build-release test valgrind asan test-all direct-reset direct-no-lonejson direct-probe direct-bench direct-callback-whitespace direct-live-heap scanner-parity-smoke scanner-parity-matrix scanner-profile-hotspots format clean

help:
	@printf '%s\n' \
	  'make build-debug   build the debug self-contained scanner foundation' \
	  'make build-release build the optimized self-contained scanner foundation' \
	  'make test          build and run the direct-stream test suite' \
	  'make valgrind      run native Valgrind Memcheck gate' \
	  'make asan          build and run direct tests with ASan/UBSan' \
	  'make test-all      run reset, dependency, test, memcheck, sanitizer, parity, and heap gates' \
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

build-debug:
	@cmake --preset debug-scanner
	@cmake --build --preset debug-scanner

build-release:
	@cmake --preset release-scanner
	@cmake --build --preset release-scanner

test:
	@cmake --preset debug-scanner
	@cmake --build --preset debug-scanner
	@ctest --test-dir build/debug-scanner --output-on-failure

asan:
	@cmake --preset asan-scanner
	@cmake --build --preset asan-scanner
	@ctest --test-dir build/asan-scanner --output-on-failure

valgrind: build-debug
	@sh scripts/check_valgrind.sh

test-all: direct-reset direct-no-lonejson test valgrind asan scanner-parity-matrix direct-callback-whitespace direct-live-heap

direct-reset:
	@sh scripts/check_direct_execution_reset.sh

direct-no-lonejson: build-debug build-release
	@sh scripts/check_no_lonejson_dependency.sh

direct-probe:
	@cmake --preset release-scanner
	@cmake --build --preset release-scanner --target lql_direct_probe

direct-bench:
	@cmake --preset release-scanner
	@cmake --build --preset release-scanner --target lql_direct_bench

direct-callback-whitespace: direct-bench
	@LQL_DIRECT_BENCH_PATH=build/release-scanner/lql_direct_bench sh scripts/check_direct_callback_whitespace.sh

direct-live-heap: direct-bench
	@LQL_DIRECT_BENCH_PATH=build/release-scanner/lql_direct_bench sh scripts/check_direct_live_heap.sh

scanner-parity-smoke: direct-bench
	@mkdir -p build
	@cd reference/go-benchmark && go build -o ../../build/reference-lqlbench ./cmd/lqlbench
	@cd reference/go-benchmark && go build -o ../../build/reference-benchvalidate ./cmd/benchvalidate
	@LQL_DIRECT_BENCH_PATH=build/release-scanner/lql_direct_bench LQL_GO_BENCH_PATH=build/reference-lqlbench LQL_BENCHVALIDATE_PATH=build/reference-benchvalidate sh scripts/check_scanner_parity_smoke.sh

scanner-parity-matrix: direct-bench
	@mkdir -p build
	@cd reference/go-benchmark && go build -o ../../build/reference-lqlbench ./cmd/lqlbench
	@cd reference/go-benchmark && go build -o ../../build/reference-benchvalidate ./cmd/benchvalidate
	@LQL_DIRECT_BENCH_PATH=build/release-scanner/lql_direct_bench LQL_GO_BENCH_PATH=build/reference-lqlbench LQL_BENCHVALIDATE_PATH=build/reference-benchvalidate sh scripts/check_scanner_parity_matrix.sh

scanner-profile-hotspots: direct-bench
	@LQL_DIRECT_BENCH_PATH=build/release-scanner/lql_direct_bench sh scripts/profile_scanner_hotspots.sh

format:
	@clang-format -i include/lql/*.h src/*.c src/*.h tests/header_smoke.c tests/header_smoke.cpp tools/lql_direct_bench.c

clean:
	@./scripts/clean.sh
