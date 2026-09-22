#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
make_bin=${MAKE:-make}

for target in \
  format-check \
  test-lifecycle \
  test-target-tools \
  test-cpkt-toolchains \
  test-all \
  asan \
  test-install-tree \
  package-source-smoke \
  release-matrix
do
  "$make_bin" -C "$repo_root" "$target"
done
