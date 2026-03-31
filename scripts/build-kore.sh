#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
deps_root=${1:-"$repo_root/.cache/deps/host-debug"}
upstream_dir="$repo_root/vendor/kore/upstream"

if [ ! -d "$upstream_dir/.git" ]; then
  echo "vendor/kore/upstream is missing; run 'make vendor-kore' first" >&2
  exit 1
fi

if [ ! -d "$deps_root/include" ] || [ ! -d "$deps_root/lib" ]; then
  echo "dependency root '$deps_root' is incomplete; run 'make deps-debug' or pass a valid deps root" >&2
  exit 1
fi

deps_root=$(CDPATH= cd -- "$deps_root" && pwd)

if [ -z "$(git -C "$upstream_dir" status --porcelain)" ]; then
  bash "$script_dir/vendor-kore.sh" apply
fi

make -C "$upstream_dir" \
  PSLOG=1 \
  LONEJSON=1 \
  CURL=1 \
  ACME=1 \
  KORE_BUNDLE_ROOT="$deps_root"
