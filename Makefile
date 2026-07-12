.PHONY: help build-debug build-release test direct-probe direct-bench scanner-parity-smoke format clean

help:
	@printf '%s\n' \
	  'make build-debug   build the selector-only reset foundation' \
	  'make build-release build the optimized selector-only foundation' \
	  'make test          build and run the direct-stream test suite' \
	  'make direct-probe  build the optimized direct-execution probe' \
	  'make direct-bench  build the optimized direct benchmark runner' \
	  'make scanner-parity-smoke  run GCC C-vs-Go scanner parity smoke' \
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

direct-probe:
	@cmake --preset release-scanner
	@cmake --build --preset release-scanner --target lql_direct_probe

direct-bench:
	@cmake --preset release-scanner
	@cmake --build --preset release-scanner --target lql_direct_bench

scanner-parity-smoke: direct-bench
	@mkdir -p build
	@cd reference/go-benchmark && go build -o ../../build/reference-lqlbench ./cmd/lqlbench
	@cd reference/go-benchmark && go build -o ../../build/reference-benchvalidate ./cmd/benchvalidate
	@LQL_DIRECT_BENCH_PATH=build/release-scanner/lql_direct_bench LQL_GO_BENCH_PATH=build/reference-lqlbench LQL_BENCHVALIDATE_PATH=build/reference-benchvalidate sh scripts/check_scanner_parity_smoke.sh

format:
	@clang-format -i include/lql/*.h src/*.c src/*.h tests/header_smoke.c tests/header_smoke.cpp tools/lql_direct_bench.c

clean:
	@./scripts/clean.sh
