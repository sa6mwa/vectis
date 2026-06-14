#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
override=${VECTIS_VERSION_OVERRIDE:-}

if [ "$#" -gt 0 ]; then
  case "$1" in
    --override)
      if [ "$#" -ne 2 ]; then
        echo "usage: scripts/release_version.sh [--override VERSION]" >&2
        exit 2
      fi
      override=$2
      ;;
    *)
      echo "usage: scripts/release_version.sh [--override VERSION]" >&2
      exit 2
      ;;
  esac
fi

validate_version() {
  case "$1" in
    ''|*[!0-9A-Za-z.+-]*)
      return 1
      ;;
  esac
  printf '%s\n' "$1" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'
}

if git_top=$(git -C "$repo_root" rev-parse --show-toplevel 2>/dev/null) &&
   [ "$(CDPATH= cd -- "$git_top" && pwd)" = "$repo_root" ]; then
  head_oid=$(git -C "$repo_root" rev-parse HEAD)
  selected=
  for tag in $(git -C "$repo_root" tag --points-at HEAD | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | sort -V); do
    if [ "$(git -C "$repo_root" cat-file -t "refs/tags/$tag")" = "commit" ] &&
       [ "$(git -C "$repo_root" rev-parse "$tag")" = "$head_oid" ]; then
      selected=${tag#v}
    fi
  done
  if [ -n "$selected" ]; then
    printf '%s\n' "$selected"
  elif [ -n "$override" ]; then
    if ! validate_version "$override"; then
      echo "invalid VECTIS_VERSION_OVERRIDE: $override" >&2
      exit 2
    fi
    printf '%s\n' "$override"
  else
    printf '0.0.0\n'
  fi
  exit 0
fi

if [ -n "$override" ]; then
  if ! validate_version "$override"; then
    echo "invalid VECTIS_VERSION_OVERRIDE: $override" >&2
    exit 2
  fi
  printf '%s\n' "$override"
  exit 0
fi

if [ -f "$repo_root/VERSION" ]; then
  version=$(sed -n '1p' "$repo_root/VERSION")
  if ! validate_version "$version"; then
    echo "invalid source archive VERSION: $version" >&2
    exit 2
  fi
  printf '%s\n' "$version"
  exit 0
fi

echo "outside git, VERSION is required for source archive builds" >&2
exit 1
