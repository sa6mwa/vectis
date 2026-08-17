#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
preset=${1:-fuzz}
cmake_bin=${CMAKE:-cmake}
build_dir="$repo_root/build/$preset"
cache="$build_dir/CMakeCache.txt"

case "$preset" in
  fuzz) ;;
  *)
    echo "configure_fuzz.sh only accepts the fuzz preset" >&2
    exit 2
    ;;
esac

if [ -f "$cache" ]; then
  if ! grep -Fq 'x86_64-linux-gnu-afl.cmake' "$cache" ||
     ! grep -Fq 'cpkt-afl-gcc' "$cache"; then
    rm -f "$cache"
    rm -rf "$build_dir/CMakeFiles"
  fi
fi

"$cmake_bin" --preset "$preset"
