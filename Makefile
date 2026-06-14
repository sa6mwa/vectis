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
	test test-debug test-lifecycle test-target-tools test-release-privacy-contracts asan test-asan coverage test-coverage fuzz fuzz-smoke test-instrumentation-presets test-install-tree test-no-kore test-e2e test-all \
	dev-up dev-down dev-reset dev-ps dev-logs \
	package package-source package-source-smoke package-checksums package-verify verify-release-archives verify-release-privacy release-matrix prerelease-hardening release print-release-version clean-dist finalize-slice prerelease \
	build-kore verify-kore-patches \
	format clean \
	vendor-kore vendor-kore-apply vendor-kore-status vendor-kore-upgrade

help:
	@printf '%s\n' \
		'make build              Configure and build the debug preset.' \
		'make test               Run the debug unit test preset.' \
		'make test-lifecycle     Run lifecycle command/version/preset/privacy contract tests.' \
		'make test-target-tools  Run target tool discovery regression tests.' \
		'make test-no-kore       Configure and link a VECTIS_WITH_KORE_RUNTIME=OFF build.' \
		'make test-e2e           Reset and run the local compose-backed lockd e2e smoke tests.' \
		'make test-all           Run unit tests and local e2e smoke tests.' \
		'make asan               Build and run the ASan/UBSan preset.' \
		'make fuzz               Configure and build the fuzz preset.' \
		'make fuzz-smoke         Build fuzz targets; bounded execution will be added with the lifecycle verifier.' \
		'make coverage           Configure and test the coverage preset.' \
		'make finalize-slice     Format and run the narrow local pre-commit gate.' \
		'make prerelease         Run deterministic local pre-release checks available in this checkout.' \
		'make print-release-version Print the version used by packaging and release targets.' \
		'make test-install-tree  Build a native installed SDK tree and verify static/shared downstream consumers.' \
		'make dev-up             Start the local lockd/MinIO/SSH/SFTP/MQTT integration environment.' \
		'make dev-down           Stop the local integration environment.' \
		'make dev-reset          Stop the local integration environment and reset lockd/MinIO generated state.' \
		'make dev-ps             Show local integration environment service status.' \
		'make dev-logs           Show local integration environment logs.' \
		'make build-release      Configure the shipped Linux release matrix.' \
		'make build-asan         Configure and build the ASan/UBSan preset.' \
		'make build-coverage     Configure and build the coverage preset.' \
		'make test-instrumentation-presets Build the sanitizer and coverage preset link-regression targets.' \
		'make build-fuzz         Configure and build the fuzz preset.' \
		'make deps-debug         Provision host debug dependencies into .cache/.' \
		'make deps-release       Provision x86_64 GNU and musl release dependencies.' \
		'make deps-cross         Provision aarch64 and armhf GNU/musl release dependencies, plus Darwin when osxcross is available.' \
		'make package            Build release SDK archives; Darwin is included only when osxcross is available.' \
		'make package-source     Build the source release archive with injected VERSION.' \
		'make package-source-smoke Extract and verify the source release archive.' \
		'make package-checksums  Generate the versioned checksum manifest for dist artifacts.' \
		'make package-verify     Verify checksum-listed artifacts, privacy, and relocatability.' \
		'make verify-release-archives Verify checksum-listed release archive layout.' \
		'make verify-release-privacy Verify release artifacts contain no local paths.' \
		'make release-matrix     Build, checksum, and verify release artifacts for supported targets.' \
		'make prerelease-hardening Run expensive deterministic hardening gates plus release matrix.' \
		'make release            Clean final tagged release artifact pipeline; refuses untagged 0.0.0.' \
		'make vendor-kore        Clone or refresh the vendored Kore upstream checkout.' \
		'make vendor-kore-apply  Assert and apply the local Kore patch series.' \
		'make vendor-kore-status Show Kore upstream revision and patch-series status.' \
		'make vendor-kore-upgrade Fetch upstream and refresh the local checkout.' \
		'make build-kore         Build patched Kore against the host debug dependency bundle.' \
		'make verify-kore-patches Non-destructively verify patch application and build from a clean Kore clone.' \
		'make clean-dist         Remove dist/ release artifacts.' \
		'make clean              Remove build/, dist/, .cache/, and generated vendor checkout state.'

build: build-debug
test: test-debug

test-lifecycle:
	$(TIMED) test-lifecycle bash ./scripts/test_lifecycle_contracts.sh
	$(TIMED) test-release-privacy-contracts bash ./scripts/test_release_privacy_contracts.sh

test-release-privacy-contracts:
	$(TIMED) test-release-privacy-contracts bash ./scripts/test_release_privacy_contracts.sh

test-target-tools:
	$(TIMED) test-target-tools bash ./scripts/test_discover_target_tools.sh

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
	@if bash ./scripts/osxcross_available.sh; then \
		$(TIMED) deps-arm64-darwin bash ./scripts/deps.sh deps-arm64-apple-darwin; \
	else \
		printf '[deps] skipping deps-arm64-apple-darwin: osxcross toolchain not available\n'; \
	fi

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
	@if bash ./scripts/osxcross_available.sh; then \
		$(TIMED) build-arm64-darwin $(CMAKE) --preset arm64-apple-darwin-release; \
		$(TIMED) build-arm64-darwin-compile $(CMAKE) --build --preset arm64-apple-darwin-release; \
	else \
		printf '[build] skipping arm64-apple-darwin-release: osxcross toolchain not available\n'; \
	fi

package:
	$(TIMED) package bash ./scripts/package.sh all

package-source:
	$(TIMED) package-source bash ./scripts/stage_release_sources.sh

package-source-smoke: package-source
	$(TIMED) package-source-smoke bash ./scripts/test_release_from_source.sh dist/vectis-$(shell bash ./scripts/release_version.sh).tar.gz

package-checksums:
	$(TIMED) package-checksums $(CMAKE) -DVECTIS_ROOT=$(ROOT) -DVECTIS_DIST_DIR=$(ROOT)/dist -P $(ROOT)/cmake/package_checksums.cmake

verify-release-archives:
	$(TIMED) verify-release-archives bash ./scripts/verify_release_artifacts.sh

verify-release-privacy:
	$(TIMED) verify-release-privacy bash ./scripts/verify_release_privacy.sh

package-verify:
	$(TIMED) package-verify bash ./scripts/package-verify.sh

release-matrix: package package-source package-checksums package-verify

prerelease-hardening: prerelease release-matrix

release:
	@if [ "$$(bash ./scripts/release_version.sh)" = "0.0.0" ]; then \
		printf '%s\n' 'make release requires an exact lightweight vX.Y.Z tag on HEAD or an explicit VECTIS_VERSION_OVERRIDE for a non-publishable rehearsal.' >&2; \
		exit 2; \
	fi
	$(MAKE) clean
	$(MAKE) release-matrix

print-release-version:
	@bash ./scripts/release_version.sh

build-asan: deps-debug
	$(TIMED) build-asan $(CMAKE) --preset $(ASAN_PRESET)
	$(TIMED) build-asan-compile $(CMAKE) --build --preset $(ASAN_PRESET)

asan: test-asan

build-coverage: deps-debug
	$(TIMED) build-coverage $(CMAKE) --preset $(COVERAGE_PRESET)
	$(TIMED) build-coverage-compile $(CMAKE) --build --preset $(COVERAGE_PRESET)

coverage: test-coverage

build-fuzz: deps-debug
	$(TIMED) build-fuzz $(CMAKE) --preset $(FUZZ_PRESET)
	$(TIMED) build-fuzz-compile $(CMAKE) --build --preset $(FUZZ_PRESET)

fuzz: build-fuzz

fuzz-smoke: build-fuzz

finalize-slice: format test-lifecycle test-target-tools test

prerelease: format test-lifecycle test-target-tools test-all asan fuzz-smoke test-install-tree package-source-smoke package-checksums package-verify

test-debug: build-debug
	$(TIMED) test-debug $(CTEST) --preset $(DEBUG_PRESET)

test-e2e:
	$(TIMED) test-e2e bash ./scripts/test-e2e.sh

test-all: test test-e2e

test-asan: build-asan
	$(TIMED) test-asan $(CTEST) --preset $(ASAN_PRESET)

test-coverage: build-coverage
	$(TIMED) test-coverage $(CTEST) --preset $(COVERAGE_PRESET)

test-instrumentation-presets: deps-debug
	$(TIMED) test-asan-preset-link $(CMAKE) --preset $(ASAN_PRESET)
	$(TIMED) test-asan-preset-link-compile $(CMAKE) --build --preset $(ASAN_PRESET) --target vectis_unit_header_cpp
	$(TIMED) test-coverage-preset-link $(CMAKE) --preset $(COVERAGE_PRESET)
	$(TIMED) test-coverage-preset-link-compile $(CMAKE) --build --preset $(COVERAGE_PRESET) --target vectis_bin

test-no-kore: deps-debug
	$(TIMED) test-no-kore bash ./scripts/test-no-kore-build.sh

test-install-tree:
	$(TIMED) deps-install-tree bash ./scripts/deps.sh deps-x86_64-linux-gnu
	$(TIMED) configure-install-tree $(CMAKE) -S . -B build/x86_64-linux-gnu-install-tree -GNinja -DCMAKE_BUILD_TYPE=Release -DVECTIS_EXTERNAL_ROOT=.cache/deps/x86_64-linux-gnu -DVECTIS_BUILD_STATIC=ON -DVECTIS_BUILD_SHARED=ON -DVECTIS_BUILD_BINARY=ON -DVECTIS_BUILD_TESTS=ON -DVECTIS_INSTALL=ON -DVECTIS_DIST_DIR=build/x86_64-linux-gnu-install-tree/dist -DVECTIS_TARGET_ARCH=x86_64 -DVECTIS_TARGET_OS=linux -DVECTIS_TARGET_LIBC=gnu
	$(TIMED) build-install-tree $(CMAKE) --build build/x86_64-linux-gnu-install-tree
	$(TIMED) package-install-tree $(CMAKE) -DVECTIS_BINARY_DIR=$(ROOT)/build/x86_64-linux-gnu-install-tree -DVECTIS_ROOT=$(ROOT) -DVECTIS_DIST_DIR=$(ROOT)/build/x86_64-linux-gnu-install-tree/dist -P $(ROOT)/cmake/package_archive.cmake
	$(TIMED) verify-install-tree-static bash ./scripts/verify_installed_sdk.sh build/x86_64-linux-gnu-install-tree/package/vectis-0.0.0-x86_64-linux-gnu static
	$(TIMED) verify-install-tree-shared bash ./scripts/verify_installed_sdk.sh build/x86_64-linux-gnu-install-tree/package/vectis-0.0.0-x86_64-linux-gnu shared

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

dev-up:
	$(TIMED) dev-up bash ./scripts/dev-up.sh

dev-down:
	$(TIMED) dev-down bash ./scripts/dev-down.sh

dev-reset:
	$(TIMED) dev-reset bash ./scripts/dev-reset.sh

dev-ps:
	bash ./scripts/dev-ps.sh

dev-logs:
	bash ./scripts/dev-logs.sh

clean:
	$(TIMED) clean bash ./scripts/clean.sh

clean-dist:
	$(TIMED) clean-dist $(CMAKE) -DVECTIS_ROOT=$(ROOT) -DVECTIS_DIST_DIR=$(ROOT)/dist -P $(ROOT)/cmake/package_clean_dist.cmake
