#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

if [ -f "$repo_root/build/devenv/devenv.yaml" ] && command -v podman >/dev/null 2>&1; then
  "$script_dir/devenv.sh" down
fi

rm -rf \
  "$repo_root/build" \
  "$repo_root/dist" \
  "$repo_root/.cache" \
  "$repo_root/vendor/kore/upstream"
