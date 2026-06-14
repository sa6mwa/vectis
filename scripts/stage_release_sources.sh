#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
version=$("$script_dir/release_version.sh")
dist_dir=${VECTIS_DIST_DIR:-"$repo_root/dist"}
stage_parent="$repo_root/build/source-stage"
stage_name="vectis-$version"
stage_root="$stage_parent/$stage_name"
manifest_tmp="$stage_parent/tracked-files.txt"
generated_manifest_tmp="$stage_parent/generated-files.txt"
archive_manifest_tmp="$stage_parent/archive-files.txt"
archive="$dist_dir/$stage_name.tar.gz"

if ! git -C "$repo_root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "source archive staging requires a git worktree" >&2
  exit 1
fi

rm -rf "$stage_parent"
mkdir -p "$stage_root" "$dist_dir"

git -C "$repo_root" ls-files --cached --others --exclude-standard | sort >"$manifest_tmp"
if [ ! -d "$repo_root/vendor/kore/upstream" ]; then
  echo "source archive staging requires vendor/kore/upstream; run 'make vendor-kore-apply' first" >&2
  exit 1
fi
find "$repo_root/vendor/kore/upstream" \
  -path "$repo_root/vendor/kore/upstream/.git" -prune -o \
  -type f -print |
  sed "s#^$repo_root/##" |
  grep -Ev '^vendor/kore/upstream/(obj/|kore$|kore\.features$|kore\.linker$|kodev/kodev$)' |
  sort >"$generated_manifest_tmp"
{
  cat "$manifest_tmp"
  cat "$generated_manifest_tmp"
} | sort -u >"$archive_manifest_tmp"
tar -C "$repo_root" -cf "$stage_parent/tracked.tar" -T "$archive_manifest_tmp"
tar -C "$stage_root" -xf "$stage_parent/tracked.tar"

printf '%s\n' "$version" >"$stage_root/VERSION"
{
  cat "$archive_manifest_tmp"
  printf '%s\n' "VERSION"
  printf '%s\n' "RELEASE_MANIFEST"
} | sort >"$stage_root/RELEASE_MANIFEST"

rm -f "$archive"
tar -C "$stage_parent" --format=gnu --owner 0 --group 0 -cf "$stage_parent/$stage_name.tar" "$stage_name"
gzip -9 -f "$stage_parent/$stage_name.tar"
mv "$stage_parent/$stage_name.tar.gz" "$archive"
printf '%s\n' "$archive"
