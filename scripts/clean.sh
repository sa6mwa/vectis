#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

rm -rf \
  "$repo_root/build" \
  "$repo_root/dist" \
  "$repo_root/.cache" \
  "$repo_root/vendor/kore/upstream"

