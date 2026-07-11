.PHONY: help build-debug build-release format clean

help:
	@printf '%s\n' \
	  'make build-debug   build the selector-only reset foundation' \
	  'make build-release build the optimized selector-only foundation' \
	  'make format        format retained C sources' \
	  'make clean         remove generated build output'

build-debug:
	@cmake --preset debug-vendored-lonejson
	@cmake --build --preset debug-vendored-lonejson

build-release:
	@cmake --preset release-vendored-lonejson
	@cmake --build --preset release-vendored-lonejson

format:
	@clang-format -i include/lql/*.h src/*.c src/*.h tests/header_smoke.c tests/header_smoke.cpp

clean:
	@./scripts/clean.sh
