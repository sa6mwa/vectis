#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
reserved_tag=v99.99.99

cleanup() {
  git -C "$repo_root" tag -d "$reserved_tag" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

fail() {
  printf 'test-release-tag-contract: %s\n' "$*" >&2
  exit 1
}

if ! git -C "$repo_root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  fail "not inside a git worktree"
fi

cleanup

existing_lightweight=
while IFS= read -r tag; do
  case "$tag" in
    v[0-9]*.[0-9]*.[0-9]*)
      if [ "$(git -C "$repo_root" cat-file -t "$tag")" = "commit" ]; then
        existing_lightweight=$tag
        break
      fi
      ;;
  esac
done <<EOF
$(git -C "$repo_root" tag --points-at HEAD)
EOF

if [ -n "$existing_lightweight" ]; then
  expected=${existing_lightweight#v}
  actual=$(make -s -C "$repo_root" print-release-version)
  if [ "$actual" != "$expected" ]; then
    fail "exact lightweight tag $existing_lightweight did not drive release version: $actual"
  fi
  printf 'release tag lifecycle contract ok\n'
  exit 0
fi

untagged=$(make -s -C "$repo_root" print-release-version)
if [ "$untagged" != "0.0.0" ]; then
  fail "untagged HEAD resolved to $untagged, expected 0.0.0"
fi

git -C "$repo_root" -c tag.gpgSign=false tag "$reserved_tag"
if [ "$(git -C "$repo_root" cat-file -t "$reserved_tag")" != "commit" ]; then
  fail "$reserved_tag is not a lightweight tag"
fi

tagged=$(make -s -C "$repo_root" print-release-version)
if [ "$tagged" != "99.99.99" ]; then
  fail "$reserved_tag resolved to $tagged, expected 99.99.99"
fi

printf 'release tag lifecycle contract ok\n'
