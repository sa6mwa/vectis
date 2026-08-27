#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
vendor_root="$repo_root/vendor/kore"
upstream_dir="$vendor_root/upstream"
patch_dir="$vendor_root/patches"
series_file="$patch_dir/series"
revision_file="$vendor_root/REVISION"
upstream_url=${VECTIS_KORE_UPSTREAM_URL:-https://git.kore.io/kore.git}
command=${1:-status}
upstream_revision=

ensure_series_file() {
  mkdir -p "$patch_dir"
  if [ ! -f "$series_file" ]; then
    : > "$series_file"
  fi
}

ensure_revision_file() {
  if [ ! -f "$revision_file" ]; then
    echo "missing Kore revision file: $revision_file" >&2
    exit 1
  fi
  upstream_revision=$(tr -d '\r\n' <"$revision_file")
  if ! [[ "$upstream_revision" =~ ^[0-9a-fA-F]{40}$ ]]; then
    echo "Kore revision must be a full 40-character Git commit: $revision_file" >&2
    exit 1
  fi
}

ensure_checkout() {
  mkdir -p "$vendor_root"
  ensure_series_file
  ensure_revision_file
  if [ ! -d "$upstream_dir/.git" ]; then
    git clone "$upstream_url" "$upstream_dir"
  fi
}

fetch_pinned_revision() {
  if git -C "$upstream_dir" cat-file -e "${upstream_revision}^{commit}" 2>/dev/null; then
    return
  fi
  git -C "$upstream_dir" fetch --no-tags origin
  if ! git -C "$upstream_dir" cat-file -e "${upstream_revision}^{commit}" 2>/dev/null; then
    echo "unable to fetch pinned Kore revision: $upstream_revision" >&2
    exit 1
  fi
}

reset_generated_checkout() {
  git -C "$upstream_dir" reset --hard
  git -C "$upstream_dir" clean -fdx
}

ensure_pinned_revision() {
  local current_revision

  fetch_pinned_revision
  current_revision=$(git -C "$upstream_dir" rev-parse HEAD)
  if [ "$current_revision" = "$upstream_revision" ]; then
    return
  fi
  git -C "$upstream_dir" checkout --detach "$upstream_revision"
}

prepare_generated_checkout() {
  ensure_checkout
  reset_generated_checkout
  ensure_pinned_revision
}

apply_series() {
  local patch_name patch_path

  prepare_generated_checkout
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
    prepare_generated_checkout
    echo "kore upstream ready at $upstream_dir"
    ;;
  apply)
    apply_series
    echo "kore patch series applied cleanly"
    ;;
  upgrade)
    ensure_checkout
    reset_generated_checkout
    git -C "$upstream_dir" fetch --no-tags origin
    ensure_pinned_revision
    echo "kore upstream refreshed at pinned revision $upstream_revision"
    ;;
  status)
    ensure_checkout
    fetch_pinned_revision
    printf 'upstream_dir=%s\n' "$upstream_dir"
    printf 'revision=%s\n' "$(git -C "$upstream_dir" rev-parse HEAD)"
    printf 'pinned_revision=%s\n' "$upstream_revision"
    printf 'branch=%s\n' "$(git -C "$upstream_dir" rev-parse --abbrev-ref HEAD)"
    printf 'patch_count=%s\n' "$(grep -vc '^[[:space:]]*#\|^[[:space:]]*$' "$series_file" || true)"
    ;;
  *)
    echo "usage: scripts/vendor-kore.sh [sync|apply|upgrade|status]" >&2
    exit 2
    ;;
esac
