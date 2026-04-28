#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
requested_abi=${1:-all}
cmake_bin=${CMAKE:-cmake}
checksum_build_dir=

unset LD_LIBRARY_PATH

run_target() {
  local preset="$1"
  local build_dir="$repo_root/build/$preset"

  bash "$script_dir/deps.sh" "deps-${preset%-release}"
  "$cmake_bin" --preset "$preset"
  "$cmake_bin" --build --preset "$preset"
  "$cmake_bin" \
    -DVECTIS_BINARY_DIR="$build_dir" \
    -DVECTIS_ROOT="$repo_root" \
    -DVECTIS_DIST_DIR="$repo_root/dist" \
    -P "$repo_root/cmake/package_archive.cmake"
  checksum_build_dir="$build_dir"
}

run_darwin_target_if_osxcross() {
  local preset="arm64-apple-darwin-release"
  local build_dir="$repo_root/build/$preset"

  if "$script_dir/osxcross_available.sh"; then
    run_target "$preset"
    "$cmake_bin" \
      -DVECTIS_BINARY_DIR="$build_dir" \
      -DVECTIS_ROOT="$repo_root" \
      -DVECTIS_DIST_DIR="$repo_root/dist" \
      -P "$repo_root/cmake/package_darwin_smoke_bundle.cmake"
  else
    printf '[package] skipping %s: osxcross toolchain not available\n' "$preset"
  fi
}

"$cmake_bin" -DVECTIS_ROOT="$repo_root" -DVECTIS_DIST_DIR="$repo_root/dist" -P "$repo_root/cmake/package_clean_dist.cmake"

case "$requested_abi" in
  all)
    run_target x86_64-linux-gnu-release
    run_target x86_64-linux-musl-release
    run_target aarch64-linux-gnu-release
    run_target aarch64-linux-musl-release
    run_target armhf-linux-gnu-release
    run_target armhf-linux-musl-release
    run_darwin_target_if_osxcross
    ;;
  gnu)
    run_target x86_64-linux-gnu-release
    run_target aarch64-linux-gnu-release
    run_target armhf-linux-gnu-release
    ;;
  musl)
    run_target x86_64-linux-musl-release
    run_target aarch64-linux-musl-release
    run_target armhf-linux-musl-release
    ;;
  x86_64-linux-gnu|x86_64-linux-musl|aarch64-linux-gnu|aarch64-linux-musl|armhf-linux-gnu|armhf-linux-musl)
    run_target "${requested_abi}-release"
    ;;
  arm64-apple-darwin)
    if ! "$script_dir/osxcross_available.sh"; then
      echo "missing arm64 Apple Darwin osxcross toolchain; set OSXCROSS_ROOT or install it under \$HOME/.local/cross/osxcross" >&2
      exit 1
    fi
    run_darwin_target_if_osxcross
    ;;
  *)
    echo "usage: scripts/package.sh [all|gnu|musl|x86_64-linux-gnu|x86_64-linux-musl|aarch64-linux-gnu|aarch64-linux-musl|armhf-linux-gnu|armhf-linux-musl|arm64-apple-darwin]" >&2
    exit 2
    ;;
esac

"$cmake_bin" \
  -DVECTIS_ROOT="$repo_root" \
  -DVECTIS_BINARY_DIR="$checksum_build_dir" \
  -DVECTIS_DIST_DIR="$repo_root/dist" \
  -P "$repo_root/cmake/package_checksums.cmake"
