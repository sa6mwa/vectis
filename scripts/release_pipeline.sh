#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
make_bin=${MAKE:-make}

for target in \
  format-check \
  test-lifecycle \
  test-target-tools \
  test-cpkt-toolchains \
  lua-test \
  test-all \
  asan \
  fuzz-smoke \
  test-install-tree \
  package-source-smoke \
  release-matrix
do
  "$make_bin" -C "$repo_root" "$target"
done
