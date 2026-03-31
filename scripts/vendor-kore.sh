#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
vendor_root="$repo_root/vendor/kore"
upstream_dir="$vendor_root/upstream"
patch_dir="$vendor_root/patches"
series_file="$patch_dir/series"
upstream_url="https://git.kore.io/kore.git"
command=${1:-status}

ensure_series_file() {
  mkdir -p "$patch_dir"
  if [ ! -f "$series_file" ]; then
    : > "$series_file"
  fi
}

ensure_checkout() {
  mkdir -p "$vendor_root"
  ensure_series_file
  if [ ! -d "$upstream_dir/.git" ]; then
    git clone "$upstream_url" "$upstream_dir"
  fi
}

ensure_clean_tree() {
  if [ -n "$(git -C "$upstream_dir" status --porcelain)" ]; then
    echo "vendor/kore/upstream has local changes; clean it before applying or upgrading" >&2
    exit 1
  fi
}

apply_series() {
  local patch_name patch_path

  ensure_checkout
  ensure_clean_tree
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
    git -C "$upstream_dir" apply --check "$patch_path"
    git -C "$upstream_dir" apply "$patch_path"
  done < "$series_file"
}

case "$command" in
  sync)
    ensure_checkout
    echo "kore upstream ready at $upstream_dir"
    ;;
  apply)
    apply_series
    echo "kore patch series applied cleanly"
    ;;
  upgrade)
    ensure_checkout
    ensure_clean_tree
    git -C "$upstream_dir" fetch origin
    git -C "$upstream_dir" pull --ff-only
    echo "kore upstream upgraded"
    ;;
  status)
    ensure_checkout
    printf 'upstream_dir=%s\n' "$upstream_dir"
    printf 'revision=%s\n' "$(git -C "$upstream_dir" rev-parse HEAD)"
    printf 'branch=%s\n' "$(git -C "$upstream_dir" rev-parse --abbrev-ref HEAD)"
    printf 'patch_count=%s\n' "$(grep -vc '^[[:space:]]*#\|^[[:space:]]*$' "$series_file" || true)"
    ;;
  *)
    echo "usage: scripts/vendor-kore.sh [sync|apply|upgrade|status]" >&2
    exit 2
    ;;
esac

