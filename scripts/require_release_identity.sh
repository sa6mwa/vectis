#!/usr/bin/env bash
set -euo pipefail
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
fail() { printf 'release identity: %s\n' "$*" >&2; exit 1; }
[[ -z "${VECTIS_VERSION_OVERRIDE:-}" ]] || fail 'candidate overrides cannot authorize a release'
git -C "$repo_root" rev-parse --is-inside-work-tree >/dev/null 2>&1 || fail 'release requires the owning Git checkout'
owner=$(git -C "$repo_root" rev-parse --show-toplevel)
[[ "$owner" == "$repo_root" ]] || fail 'release root does not own this Git checkout'
default_ref=$(git -C "$repo_root" symbolic-ref refs/remotes/origin/HEAD) || fail 'resolve origin default branch first'
default_branch=${default_ref#refs/remotes/origin/}
branch=$(git -C "$repo_root" symbolic-ref --short HEAD) || fail 'release requires an attached default branch'
[[ "$branch" == "$default_branch" ]] || fail "release requires $default_branch; current branch is $branch"
"$script_dir/require_clean_release_worktree.sh"
git -C "$repo_root" verify-commit HEAD >/dev/null 2>&1 || fail 'HEAD must have a verified commit signature'
version=$("$script_dir/release_version.sh")
[[ "$version" != 0.0.0 ]] || fail 'HEAD requires its exact lightweight vX.Y.Z release tag'
tag="refs/tags/v$version"
[[ "$(git -C "$repo_root" cat-file -t "$tag")" == commit ]] || fail 'release tag must be lightweight'
[[ "$(git -C "$repo_root" rev-parse "$tag")" == "$(git -C "$repo_root" rev-parse HEAD)" ]] || fail 'release tag must identify signed HEAD'
printf 'Release identity verified: %s v%s %s\n' "$branch" "$version" "$(git -C "$repo_root" rev-parse HEAD)"
