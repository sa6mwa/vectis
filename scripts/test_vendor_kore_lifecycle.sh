#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
mkdir -p "$repo_root/build"
work_root=$(mktemp -d "$repo_root/build/vendor-kore-lifecycle.XXXXXX")
origin_dir="$work_root/origin.git"
source_dir="$work_root/source"
fixture_root="$work_root/fixture"

cleanup() {
  rm -rf "$work_root"
}
trap cleanup EXIT INT TERM

fail() {
  echo "$*" >&2
  exit 1
}

assert_file_value() {
  local path expected actual

  path=$1
  expected=$2
  actual=$(cat "$path")
  [ "$actual" = "$expected" ] ||
    fail "expected $path to contain '$expected', got '$actual'"
}

write_patch() {
  local content

  content=$1
  printf '%s\n' "$content" >"$source_dir/base.txt"
  git -C "$source_dir" diff -- base.txt >"$fixture_root/vendor/kore/patches/0001.patch"
  git -C "$source_dir" checkout -- base.txt
}

run_vendor() {
  VECTIS_KORE_UPSTREAM_URL="$origin_dir" \
    bash "$fixture_root/scripts/vendor-kore.sh" "$1" >/dev/null
}

git init --bare --initial-branch=main "$origin_dir" >/dev/null
git init --initial-branch=main "$source_dir" >/dev/null
git -C "$source_dir" config user.email "vectis-test@example.com"
git -C "$source_dir" config user.name "Vectis test"
printf 'base\n' >"$source_dir/base.txt"
printf 'initial\n' >"$source_dir/next.txt"
git -C "$source_dir" add base.txt next.txt
git -C "$source_dir" commit -m "initial Kore fixture" >/dev/null
git -C "$source_dir" remote add origin "$origin_dir"
git -C "$source_dir" push -u origin main >/dev/null
revision_a=$(git -C "$source_dir" rev-parse HEAD)

mkdir -p "$fixture_root/scripts" "$fixture_root/vendor/kore/patches"
cp "$repo_root/scripts/vendor-kore.sh" "$fixture_root/scripts/vendor-kore.sh"
printf '%s\n' "$revision_a" >"$fixture_root/vendor/kore/REVISION"
printf '%s\n' '0001.patch' >"$fixture_root/vendor/kore/patches/series"

write_patch 'patched-one'
run_vendor apply
assert_file_value "$fixture_root/vendor/kore/upstream/base.txt" 'patched-one'

touch "$fixture_root/vendor/kore/upstream/generated-file"
write_patch 'patched-two'
run_vendor apply
assert_file_value "$fixture_root/vendor/kore/upstream/base.txt" 'patched-two'
[ ! -e "$fixture_root/vendor/kore/upstream/generated-file" ] ||
  fail 'apply did not remove generated checkout files before reapplying patches'

printf 'advanced\n' >"$source_dir/next.txt"
git -C "$source_dir" add next.txt
git -C "$source_dir" commit -m "advance Kore fixture" >/dev/null
git -C "$source_dir" push >/dev/null
revision_b=$(git -C "$source_dir" rev-parse HEAD)
printf '%s\n' "$revision_b" >"$fixture_root/vendor/kore/REVISION"

run_vendor apply
[ "$(git -C "$fixture_root/vendor/kore/upstream" rev-parse HEAD)" = "$revision_b" ] ||
  fail 'apply did not fetch and check out the advanced pinned revision'
assert_file_value "$fixture_root/vendor/kore/upstream/base.txt" 'patched-two'
assert_file_value "$fixture_root/vendor/kore/upstream/next.txt" 'advanced'

run_vendor upgrade
[ -z "$(git -C "$fixture_root/vendor/kore/upstream" status --porcelain)" ] ||
  fail 'upgrade left the generated checkout dirty'
[ "$(git -C "$fixture_root/vendor/kore/upstream" rev-parse HEAD)" = "$revision_b" ] ||
  fail 'upgrade did not retain the advanced pinned revision'
assert_file_value "$fixture_root/vendor/kore/upstream/base.txt" 'base'
assert_file_value "$fixture_root/vendor/kore/upstream/next.txt" 'advanced'

echo 'vendor Kore lifecycle regression test ok'
