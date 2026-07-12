.PHONY: help build-debug build-release test direct-probe direct-bench format clean

help:
	@printf '%s\n' \
	  'make build-debug   build the selector-only reset foundation' \
	  'make build-release build the optimized selector-only foundation' \
	  'make test          build and run the direct-stream test suite' \
	  'make direct-probe  build the optimized direct-execution probe' \
	  'make direct-bench  build the optimized direct benchmark runner' \
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

format:
	@clang-format -i include/lql/*.h src/*.c src/*.h tests/header_smoke.c tests/header_smoke.cpp tools/lql_direct_bench.c

clean:
	@./scripts/clean.sh
