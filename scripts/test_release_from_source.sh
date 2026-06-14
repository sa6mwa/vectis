#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
archive=${1:?usage: scripts/test_release_from_source.sh ARCHIVE}
cmake_bin=${CMAKE:-cmake}
work_root="$repo_root/build/source-smoke"
extract_root="$work_root/extract"

case "$archive" in
  /*) ;;
  *) archive="$repo_root/$archive" ;;
esac

rm -rf "$work_root"
mkdir -p "$extract_root"
tar -C "$extract_root" -xzf "$archive"

root_count=$(find "$extract_root" -mindepth 1 -maxdepth 1 -type d | wc -l | tr -d ' ')
if [ "$root_count" != "1" ]; then
  echo "source archive must contain exactly one root directory" >&2
  exit 1
fi
source_root=$(find "$extract_root" -mindepth 1 -maxdepth 1 -type d | sort | sed -n '1p')

if [ -d "$source_root/.git" ]; then
  echo "source archive contains .git" >&2
  exit 1
fi
if [ ! -f "$source_root/VERSION" ]; then
  echo "source archive missing injected VERSION" >&2
  exit 1
fi
if [ ! -f "$source_root/RELEASE_MANIFEST" ]; then
  echo "source archive missing RELEASE_MANIFEST" >&2
  exit 1
fi

version=$(sed -n '1p' "$source_root/VERSION")
root_name=$(basename "$source_root")
if [ "$root_name" != "vectis-$version" ]; then
  echo "source archive root/version mismatch: root=$root_name version=$version" >&2
  exit 1
fi

actual_manifest="$work_root/actual-files.txt"
expected_manifest="$work_root/expected-files.txt"
find "$source_root" -type f | sed "s#^$source_root/##" | sort >"$actual_manifest"
sort "$source_root/RELEASE_MANIFEST" >"$expected_manifest"
if ! cmp -s "$actual_manifest" "$expected_manifest"; then
  echo "source archive manifest does not match payload" >&2
  diff -u "$expected_manifest" "$actual_manifest" || true
  exit 1
fi

if [ "${VECTIS_SOURCE_SMOKE_BUILD:-1}" != "0" ]; then
  bash "$source_root/scripts/deps.sh" deps-host-debug
  "$cmake_bin" -S "$source_root" -B "$work_root/build" -GNinja
  "$cmake_bin" --build "$work_root/build"
  "$cmake_bin" --build "$work_root/build" --target test
fi

echo "source archive smoke ok"
