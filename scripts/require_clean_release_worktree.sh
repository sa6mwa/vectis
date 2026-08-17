#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

if ! git -C "$repo_root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  exit 0
fi

if ! git -C "$repo_root" diff --quiet -- .; then
  echo "release requires a clean tracked worktree before cleanup or packaging" >&2
  exit 1
fi
if ! git -C "$repo_root" diff --cached --quiet -- .; then
  echo "release requires a clean index before cleanup or packaging" >&2
  exit 1
fi
if [ -n "$(git -C "$repo_root" ls-files --others --exclude-standard)" ]; then
  echo "release requires no untracked non-ignored files before cleanup or packaging" >&2
  exit 1
fi
