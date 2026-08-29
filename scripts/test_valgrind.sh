#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "usage: scripts/test_valgrind.sh BUILD_DIR" >&2
  exit 2
fi

build_dir=$1
valgrind_bin=${VALGRIND:-valgrind}
timeout_bin=${TIMEOUT:-timeout}
unit_dir="$build_dir/tests/unit"
# Keep Memcheck focused on native Vectis façades. The complete unit suite runs
# under the normal and ASan gates; dependency-heavy cryptography and model
# fixtures would make the prerelease memory gate impractically slow.
tests='vectis_unit_config
vectis_unit_auth
vectis_unit_embedded_fs
vectis_unit_webdav
vectis_unit_sdk_surface'

if ! command -v "$valgrind_bin" >/dev/null 2>&1; then
  echo "valgrind is required for the native memory-check gate" >&2
  exit 2
fi
if ! command -v "$timeout_bin" >/dev/null 2>&1; then
  echo "timeout is required for the native memory-check gate" >&2
  exit 2
fi

for test_name in $tests; do
  test_path="$unit_dir/$test_name"
  if [ ! -x "$test_path" ]; then
    echo "valgrind test binary is missing or not executable: $test_path" >&2
    exit 2
  fi
  printf '[valgrind] %s\n' "$test_name"
  "$timeout_bin" --preserve-status 240 "$valgrind_bin" \
    --leak-check=full \
    --show-leak-kinds=definite,indirect,possible \
    --errors-for-leak-kinds=definite,indirect,possible \
    --track-origins=yes \
    --error-exitcode=99 \
    --num-callers=30 \
    "$test_path"
done
