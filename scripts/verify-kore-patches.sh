#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
deps_root=${1:-"$repo_root/.cache/deps/host-debug"}
source_repo="$repo_root/vendor/kore/upstream"
patch_dir="$repo_root/vendor/kore/patches"
series_file="$patch_dir/series"
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/vectis-kore-verify.XXXXXX")

cleanup() {
  rm -rf "$tmpdir"
}
trap cleanup EXIT INT TERM

if [ ! -d "$source_repo/.git" ]; then
  echo "vendor/kore/upstream is missing; run 'make vendor-kore' first" >&2
  exit 1
fi

if [ ! -f "$series_file" ]; then
  echo "missing patch series file: $series_file" >&2
  exit 1
fi

if [ ! -d "$deps_root/include" ] || [ ! -d "$deps_root/lib" ]; then
  echo "dependency root '$deps_root' is incomplete; run 'make deps-debug' or pass a valid deps root" >&2
  exit 1
fi

deps_root=$(CDPATH= cd -- "$deps_root" && pwd)

git clone "$source_repo" "$tmpdir/upstream" >/dev/null

while IFS= read -r patch_name; do
  [ -z "$patch_name" ] && continue
  case "$patch_name" in
    \#*) continue ;;
  esac
  patch_path="$patch_dir/$patch_name"
  if [ ! -f "$patch_path" ]; then
    echo "missing patch file: $patch_path" >&2
    exit 1
  fi
  git -C "$tmpdir/upstream" apply --check "$patch_path"
  git -C "$tmpdir/upstream" apply "$patch_path"
done < "$series_file"

make -C "$tmpdir/upstream" \
  PSLOG=1 \
  LONEJSON=1 \
  CURL=1 \
  ACME=1 \
  KORE_BUNDLE_ROOT="$deps_root"
