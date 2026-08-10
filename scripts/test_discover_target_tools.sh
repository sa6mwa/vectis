#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
work="$repo_root/build/test-discover-target-tools"

rm -rf "$work"
mkdir -p "$work"

cleanup() {
  rm -rf "$work"
}
trap cleanup EXIT INT TERM

make_tool() {
  path=$1
  mkdir -p "$(dirname "$path")"
  printf '#!/bin/sh\nexit 0\n' >"$path"
  chmod +x "$path"
}

assert_output_contains() {
  output=$1
  expected=$2
  if ! printf '%s\n' "$output" | grep -F "$expected" >/dev/null; then
    echo "target tool discovery missing: $expected" >&2
    printf '%s\n' "$output" >&2
    exit 1
  fi
}

configured_case() {
  root="$work/configured"
  build="$root/build"
  mkdir -p "$build"
  make_tool "$root/bin/cc"
  make_tool "$root/bin/custom-strip"
  make_tool "$root/bin/custom-install-name-tool"
  make_tool "$root/bin/custom-otool"
  make_tool "$root/bin/custom-readelf"
  cat >"$build/CMakeCache.txt" <<EOF
CMAKE_C_COMPILER:FILEPATH=$root/bin/cc
CMAKE_STRIP:FILEPATH=$root/bin/custom-strip
CMAKE_INSTALL_NAME_TOOL:FILEPATH=$root/bin/custom-install-name-tool
CMAKE_OTOOL:FILEPATH=$root/bin/custom-otool
CMAKE_READELF:FILEPATH=$root/bin/custom-readelf
EOF
  output=$("$repo_root/scripts/discover_target_tools.sh" --build-dir "$build" --target-id arm64-apple-darwin)
  assert_output_contains "$output" "STRIP='$root/bin/custom-strip'"
  assert_output_contains "$output" "INSTALL_NAME_TOOL='$root/bin/custom-install-name-tool'"
  assert_output_contains "$output" "OTOOL='$root/bin/custom-otool'"
  assert_output_contains "$output" "READELF='$root/bin/custom-readelf'"
}

darwin_sibling_case() {
  root="$work/darwin-sibling"
  toolbin="$root/toolchain/bin"
  build="$root/build"
  mkdir -p "$toolbin" "$build"
  for tool in arm64-apple-darwin25-clang arm64-apple-darwin25-strip arm64-apple-darwin25-install_name_tool arm64-apple-darwin25-otool readelf; do
    make_tool "$toolbin/$tool"
  done
  cat >"$build/CMakeCache.txt" <<EOF
CMAKE_C_COMPILER:FILEPATH=$toolbin/arm64-apple-darwin25-clang
CMAKE_STRIP:FILEPATH=
CMAKE_INSTALL_NAME_TOOL:FILEPATH=
EOF
  output=$("$repo_root/scripts/discover_target_tools.sh" --build-dir "$build" --target-id arm64-apple-darwin)
  assert_output_contains "$output" "CC='$toolbin/arm64-apple-darwin25-clang'"
  assert_output_contains "$output" "STRIP='$toolbin/arm64-apple-darwin25-strip'"
  assert_output_contains "$output" "INSTALL_NAME_TOOL='$toolbin/arm64-apple-darwin25-install_name_tool'"
  assert_output_contains "$output" "OTOOL='$toolbin/arm64-apple-darwin25-otool'"
  assert_output_contains "$output" "TARGET_HOST_PREFIX='arm64-apple-darwin25'"
}

linux_path_fallback_case() {
  root="$work/linux-path"
  pathbin="$root/path/bin"
  build="$root/build"
  mkdir -p "$pathbin" "$build"
  make_tool "$root/cc"
  make_tool "$pathbin/x86_64-linux-gnu-strip"
  make_tool "$pathbin/x86_64-linux-gnu-readelf"
  cat >"$build/CMakeCache.txt" <<EOF
CMAKE_C_COMPILER:FILEPATH=$root/cc
CMAKE_STRIP:FILEPATH=
CMAKE_READELF:FILEPATH=
EOF
  output=$(PATH="$pathbin:$PATH" "$repo_root/scripts/discover_target_tools.sh" --build-dir "$build" --target-id x86_64-linux-gnu)
  assert_output_contains "$output" "STRIP='$pathbin/x86_64-linux-gnu-strip'"
  assert_output_contains "$output" "READELF='$pathbin/x86_64-linux-gnu-readelf'"
}

darwin_refuses_host_tools_case() {
  root="$work/darwin-host-refusal"
  pathbin="$root/path/bin"
  build="$root/build"
  mkdir -p "$pathbin" "$build"
  make_tool "$root/arm64-apple-darwin25-clang"
  make_tool "$pathbin/strip"
  make_tool "$pathbin/install_name_tool"
  make_tool "$pathbin/otool"
  cat >"$build/CMakeCache.txt" <<EOF
CMAKE_C_COMPILER:FILEPATH=$root/arm64-apple-darwin25-clang
CMAKE_STRIP:FILEPATH=
CMAKE_INSTALL_NAME_TOOL:FILEPATH=
CMAKE_OTOOL:FILEPATH=
EOF
  output=$(PATH="$pathbin:/usr/bin:/bin" "$repo_root/scripts/discover_target_tools.sh" --build-dir "$build" --target-id arm64-apple-darwin)
  assert_output_contains "$output" "STRIP=''"
  assert_output_contains "$output" "INSTALL_NAME_TOOL=''"
  assert_output_contains "$output" "OTOOL=''"
}

configured_case
darwin_sibling_case
linux_path_fallback_case
darwin_refuses_host_tools_case

echo "target tool discovery ok"
