#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_dir=${1:-"$repo_root/build/fuzz"}
tmp_dir=${TMPDIR:-/tmp}/vectis-fuzz-smoke.$$

cleanup() {
  rm -rf "$tmp_dir"
}
trap cleanup EXIT INT TERM

value() {
  sed -n "s/^$1=//p" | tail -1
}

afl_showmap=$("$script_dir/cpkt-aflpp.sh" discover | value afl_showmap)
json_target="$build_dir/tests/fuzz/vectis_fuzz_json_validate"
kore_target="$build_dir/tests/fuzz/vectis_fuzz_kore_bridge"

if [ ! -x "$json_target" ]; then
  echo "missing fuzz target: $json_target" >&2
  exit 1
fi
if [ ! -x "$kore_target" ]; then
  echo "missing fuzz target: $kore_target" >&2
  exit 1
fi

mkdir -p "$tmp_dir"
printf '%s\n' '{"ok":true}' >"$tmp_dir/json.seed"
AFL_QUIET=1 "$afl_showmap" -o "$tmp_dir/json.map" -- "$json_target" \
  <"$tmp_dir/json.seed" >/dev/null

for seed in "$repo_root"/tests/fuzz/corpus/kore_bridge/*; do
  seed_name=$(basename -- "$seed")
  AFL_QUIET=1 "$afl_showmap" -o "$tmp_dir/kore-$seed_name.map" -- \
    "$kore_target" <"$seed" >/dev/null
done
