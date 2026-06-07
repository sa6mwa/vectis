#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_dir="$repo_root/build/no-kore"

cmake -S "$repo_root" -B "$build_dir" -GNinja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DVECTIS_EXTERNAL_ROOT="$repo_root/.cache/deps/host-debug" \
  -DVECTIS_WITH_KORE_RUNTIME=OFF \
  -DVECTIS_BUILD_STATIC=ON \
  -DVECTIS_BUILD_SHARED=OFF \
  -DVECTIS_BUILD_BINARY=ON \
  -DVECTIS_BUILD_TESTS=OFF \
  -DVECTIS_BUILD_FUZZERS=OFF \
  -DVECTIS_INSTALL=OFF

cmake --build "$build_dir"
