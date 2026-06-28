#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
requested_abi=${1:-all}
cmake_bin=${CMAKE:-cmake}
checksum_build_dir=

unset LD_LIBRARY_PATH

verify_native_install_tree() {
  local preset="$1"
  local build_dir="$2"
  local package_root

  if [ "$(uname -s)" != "Linux" ] || [ "$(uname -m)" != "x86_64" ]; then
    return
  fi
  if [ "$preset" != "x86_64-linux-gnu-release" ]; then
    return
  fi

  package_root=$(find "$build_dir/package" -maxdepth 1 -type d -name 'vectis-*' | sort | tail -n 1)
  if [ -z "$package_root" ]; then
    echo "[package] missing staged install tree for $preset" >&2
    exit 1
  fi
  if [ -f "$package_root/lib/libvectis.a" ]; then
    bash "$script_dir/verify_installed_sdk.sh" "$package_root" static
  fi
  if [ -f "$package_root/lib/libvectis.so" ]; then
    bash "$script_dir/verify_installed_sdk.sh" "$package_root" shared
  fi
  if [ ! -x "$package_root/bin/vectis" ]; then
    echo "[package] missing executable vectis binary for $preset" >&2
    exit 1
  fi
  LD_LIBRARY_PATH="$package_root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$package_root/bin/vectis" --version >/dev/null
}

target_id_for_preset() {
  local preset="$1"
  printf '%s\n' "${preset%-release}"
}

emit_toolchain_skip() {
  local target_id="$1"
  printf '[package] skipping %s: target toolchain not available\n' "$target_id" >&2
  cat >&2 <<EOF
PKT_DIAGNOSTIC_BEGIN
surface=package
phase=target-toolchain
status=skipped
class=external-tool-unavailable
reason=target toolchain not available
artifact=$target_id
next=install the target compiler, ar, and ranlib tools or request a narrower package target
PKT_DIAGNOSTIC_END
EOF
}

require_target_toolchain() {
  local target_id="$1"
  if ! "$script_dir/target_toolchain_available.sh" "$target_id"; then
    cat >&2 <<EOF
PKT_DIAGNOSTIC_BEGIN
surface=package
phase=target-toolchain
status=failed
class=external-tool-unavailable
reason=target toolchain not available
artifact=$target_id
next=install the target compiler, ar, and ranlib tools or request a target available on this host
PKT_DIAGNOSTIC_END
EOF
    exit 1
  fi
}

run_optional_target() {
  local preset="$1"
  local target_id

  target_id=$(target_id_for_preset "$preset")
  if "$script_dir/target_toolchain_available.sh" "$target_id"; then
    run_target "$preset"
  else
    emit_toolchain_skip "$target_id"
  fi
}

run_target() {
  local preset="$1"
  local build_dir="$repo_root/build/$preset"
  local tool_args=()

  bash "$script_dir/deps.sh" "deps-${preset%-release}"
  "$cmake_bin" --preset "$preset"
  "$cmake_bin" --build --preset "$preset"
  eval "$("$script_dir/discover_target_tools.sh" --build-dir "$build_dir" --target-id "${preset%-release}")"
  if [ -n "${STRIP:-}" ]; then
    tool_args+=("-DCMAKE_STRIP=$STRIP")
  fi
  if [ -n "${OTOOL:-}" ]; then
    tool_args+=("-DVECTIS_OTOOL=$OTOOL")
  fi
  "$cmake_bin" \
    -DVECTIS_BINARY_DIR="$build_dir" \
    -DVECTIS_ROOT="$repo_root" \
    -DVECTIS_DIST_DIR="$repo_root/dist" \
    "${tool_args[@]}" \
    -P "$repo_root/cmake/package_archive.cmake"
  verify_native_install_tree "$preset" "$build_dir"
  checksum_build_dir="$build_dir"
}

run_darwin_target_if_osxcross() {
  local preset="arm64-apple-darwin-release"
  local build_dir="$repo_root/build/$preset"

  if "$script_dir/osxcross_available.sh"; then
    run_target "$preset"
    eval "$("$script_dir/discover_target_tools.sh" --build-dir "$build_dir" --target-id arm64-apple-darwin)"
    "$cmake_bin" \
      -DVECTIS_BINARY_DIR="$build_dir" \
      -DVECTIS_ROOT="$repo_root" \
      -DVECTIS_DIST_DIR="$repo_root/dist" \
      -DVECTIS_OTOOL="$OTOOL" \
      -P "$repo_root/cmake/package_darwin_smoke_bundle.cmake"
  else
    emit_toolchain_skip arm64-apple-darwin
  fi
}

"$cmake_bin" -DVECTIS_ROOT="$repo_root" -DVECTIS_DIST_DIR="$repo_root/dist" -P "$repo_root/cmake/package_clean_dist.cmake"

case "$requested_abi" in
  all)
    run_optional_target x86_64-linux-gnu-release
    run_optional_target x86_64-linux-musl-release
    run_optional_target aarch64-linux-gnu-release
    run_optional_target aarch64-linux-musl-release
    run_optional_target armhf-linux-gnu-release
    run_optional_target armhf-linux-musl-release
    run_darwin_target_if_osxcross
    ;;
  gnu)
    run_optional_target x86_64-linux-gnu-release
    run_optional_target aarch64-linux-gnu-release
    run_optional_target armhf-linux-gnu-release
    ;;
  musl)
    run_optional_target x86_64-linux-musl-release
    run_optional_target aarch64-linux-musl-release
    run_optional_target armhf-linux-musl-release
    ;;
  x86_64-linux-gnu|x86_64-linux-musl|aarch64-linux-gnu|aarch64-linux-musl|armhf-linux-gnu|armhf-linux-musl)
    require_target_toolchain "$requested_abi"
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
