SHELL := bash
.DEFAULT_GOAL := help
MAKEFLAGS += --no-builtin-rules

ROOT := $(CURDIR)
CMAKE := cmake
CTEST := ctest
TIMED := bash ./scripts/run_timed.sh

DEBUG_PRESET := debug
ASAN_PRESET := asan
COVERAGE_PRESET := coverage
FUZZ_PRESET := fuzz

.PHONY: \
	help \
	deps-debug deps-release deps-cross \
	build build-debug build-release build-asan build-coverage build-fuzz \
	test test-debug test-asan test-coverage \
	build-kore verify-kore-patches \
	format clean \
	vendor-kore vendor-kore-apply vendor-kore-status vendor-kore-upgrade

help:
	@printf '%s\n' \
		'make build              Configure and build the debug preset.' \
		'make test               Run the debug unit test preset.' \
		'make build-release      Configure the shipped Linux release matrix.' \
		'make build-asan         Configure and build the ASan/UBSan preset.' \
		'make build-coverage     Configure and build the coverage preset.' \
		'make build-fuzz         Configure and build the fuzz preset.' \
		'make deps-debug         Provision host debug dependencies into .cache/.' \
		'make deps-release       Provision x86_64 GNU and musl release dependencies.' \
		'make deps-cross         Provision aarch64 and armhf GNU/musl release dependencies.' \
		'make vendor-kore        Clone or refresh the vendored Kore upstream checkout.' \
		'make vendor-kore-apply  Assert and apply the local Kore patch series.' \
		'make vendor-kore-status Show Kore upstream revision and patch-series status.' \
		'make vendor-kore-upgrade Fetch upstream and refresh the local checkout.' \
		'make build-kore         Build patched Kore against the host debug dependency bundle.' \
		'make verify-kore-patches Non-destructively verify patch application and build from a clean Kore clone.' \
		'make clean              Remove build/, dist/, .cache/, and generated vendor checkout state.'

build: build-debug
test: test-debug

deps-debug:
	$(TIMED) deps-debug bash ./scripts/deps.sh deps-host-debug

deps-release:
	$(TIMED) deps-release bash ./scripts/deps.sh deps-x86_64-linux-gnu
	$(TIMED) deps-release-musl bash ./scripts/deps.sh deps-x86_64-linux-musl

deps-cross:
	$(TIMED) deps-aarch64-gnu bash ./scripts/deps.sh deps-aarch64-linux-gnu
	$(TIMED) deps-aarch64-musl bash ./scripts/deps.sh deps-aarch64-linux-musl
	$(TIMED) deps-armhf-gnu bash ./scripts/deps.sh deps-armhf-linux-gnu
	$(TIMED) deps-armhf-musl bash ./scripts/deps.sh deps-armhf-linux-musl

build-debug: deps-debug
	$(TIMED) build-debug $(CMAKE) --preset $(DEBUG_PRESET)
	$(TIMED) build-debug-compile $(CMAKE) --build --preset $(DEBUG_PRESET)

build-release: deps-release deps-cross
	$(TIMED) build-x86_64-gnu $(CMAKE) --preset x86_64-linux-gnu-release
	$(TIMED) build-x86_64-gnu-compile $(CMAKE) --build --preset x86_64-linux-gnu-release
	$(TIMED) build-x86_64-musl $(CMAKE) --preset x86_64-linux-musl-release
	$(TIMED) build-x86_64-musl-compile $(CMAKE) --build --preset x86_64-linux-musl-release
	$(TIMED) build-aarch64-gnu $(CMAKE) --preset aarch64-linux-gnu-release
	$(TIMED) build-aarch64-gnu-compile $(CMAKE) --build --preset aarch64-linux-gnu-release
	$(TIMED) build-aarch64-musl $(CMAKE) --preset aarch64-linux-musl-release
	$(TIMED) build-aarch64-musl-compile $(CMAKE) --build --preset aarch64-linux-musl-release
	$(TIMED) build-armhf-gnu $(CMAKE) --preset armhf-linux-gnu-release
	$(TIMED) build-armhf-gnu-compile $(CMAKE) --build --preset armhf-linux-gnu-release
	$(TIMED) build-armhf-musl $(CMAKE) --preset armhf-linux-musl-release
	$(TIMED) build-armhf-musl-compile $(CMAKE) --build --preset armhf-linux-musl-release

build-asan: deps-debug
	$(TIMED) build-asan $(CMAKE) --preset $(ASAN_PRESET)
	$(TIMED) build-asan-compile $(CMAKE) --build --preset $(ASAN_PRESET)

build-coverage: deps-debug
	$(TIMED) build-coverage $(CMAKE) --preset $(COVERAGE_PRESET)
	$(TIMED) build-coverage-compile $(CMAKE) --build --preset $(COVERAGE_PRESET)

build-fuzz: deps-debug
	$(TIMED) build-fuzz $(CMAKE) --preset $(FUZZ_PRESET)
	$(TIMED) build-fuzz-compile $(CMAKE) --build --preset $(FUZZ_PRESET)

test-debug: build-debug
	$(TIMED) test-debug $(CTEST) --preset $(DEBUG_PRESET)

test-asan: build-asan
	$(TIMED) test-asan $(CTEST) --preset $(ASAN_PRESET)

test-coverage: build-coverage
	$(TIMED) test-coverage $(CTEST) --preset $(COVERAGE_PRESET)

format:
	rg --files -g '*.c' -g '*.h' | xargs clang-format -i

vendor-kore:
	$(TIMED) vendor-kore bash ./scripts/vendor-kore.sh sync

vendor-kore-apply:
	$(TIMED) vendor-kore-apply bash ./scripts/vendor-kore.sh apply

vendor-kore-status:
	bash ./scripts/vendor-kore.sh status

vendor-kore-upgrade:
	$(TIMED) vendor-kore-upgrade bash ./scripts/vendor-kore.sh upgrade

build-kore: deps-debug vendor-kore
	$(TIMED) build-kore bash ./scripts/build-kore.sh ./.cache/deps/host-debug

verify-kore-patches: deps-debug vendor-kore
	$(TIMED) verify-kore-patches bash ./scripts/verify-kore-patches.sh ./.cache/deps/host-debug

clean:
	$(TIMED) clean bash ./scripts/clean.sh
