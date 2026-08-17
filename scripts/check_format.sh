#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

cd "$repo_root"
if ! command -v clang-format >/dev/null 2>&1; then
  echo "clang-format is required for make format-check" >&2
  exit 2
fi

rg --files -g '*.c' -g '*.h' |
  xargs clang-format --dry-run --Werror
