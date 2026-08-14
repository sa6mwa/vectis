#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
dist_dir=${1:-${VECTIS_DIST_DIR:-"$repo_root/dist"}}
version=${2:-${VECTIS_VERSION:-$("$script_dir/release_version.sh")}}
work_root="$repo_root/build/luarocks-validate"
package_name="vectis-lua-$version"
archive="$dist_dir/$package_name.tar.gz"
rockspec="$dist_dir/vectis-$version-1.rockspec"
src_rock="$dist_dir/vectis-$version-1.src.rock"
luarocks=${LUAROCKS:-luarocks}

fail() {
  reason=$1
  artifact=${2:-}
  printf '%s%s\n' "$reason" "${artifact:+: $artifact}" >&2
  cat >&2 <<EOF
PKT_DIAGNOSTIC_BEGIN
surface=validate-luarocks
phase=lua-release-artifact-validation
status=failed
class=package-layout
reason=$reason
artifact=$artifact
next=regenerate Lua artifacts with make release-lua-artifacts
PKT_DIAGNOSTIC_END
EOF
  exit 1
}

for artifact in "$archive" "$rockspec" "$src_rock"; do
  [ -f "$artifact" ] || fail "missing Lua release artifact" "$artifact"
done

scan_no_private_paths() {
  file=$1
  if strings -a "$file" | grep -F "file://$repo_root" >/dev/null 2>&1; then
    fail "repository file URL leaked into Lua artifact" "$file"
  fi
  if [ -n "${HOME:-}" ] &&
     strings -a "$file" | grep -F "file://${HOME:-}" >/dev/null 2>&1; then
    fail "home file URL leaked into Lua artifact" "$file"
  fi
  if strings -a "$file" | grep -F "$repo_root" >/dev/null 2>&1; then
    fail "repository path leaked into Lua artifact" "$file"
  fi
  if [ -n "${HOME:-}" ] &&
     strings -a "$file" | grep -F "${HOME:-}" >/dev/null 2>&1; then
    fail "home path leaked into Lua artifact" "$file"
  fi
}

check_rockspec() {
  file=$1
  grep -F "version = \"$version-1\"" "$file" >/dev/null ||
    fail "Lua rockspec version mismatch" "$file"
  grep -F "url = \"$package_name.tar.gz\"" "$file" >/dev/null ||
    fail "Lua rockspec source URL mismatch" "$file"
  grep -F "dir = \"$package_name\"" "$file" >/dev/null ||
    fail "Lua rockspec source dir mismatch" "$file"
  grep -E 'url = "file://|url = "/' "$file" >/dev/null &&
    fail "Lua rockspec contains a local source URL" "$file"
  scan_no_private_paths "$file"
}

check_lua_module_files() {
  root=$1
  module_rockspec=$2
  artifact=$3
  manifest="$root/RELEASE_MANIFEST"

  [ -f "$manifest" ] || fail "Lua source archive missing RELEASE_MANIFEST" "$artifact"
  sed -n 's/^[[:space:]]*\["[^"]*"\][[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' \
    "$module_rockspec" |
  while IFS= read -r module_path; do
    [ -n "$module_path" ] || continue
    case "$module_path" in
      /*|*../*|../*)
        fail "Lua rockspec module path is not package-relative" "$module_path"
        ;;
    esac
    [ -f "$root/$module_path" ] ||
      fail "Lua source archive missing rockspec module file" "$module_path"
    grep -Fx "$module_path" "$manifest" >/dev/null ||
      fail "Lua source archive manifest missing rockspec module file" "$module_path"
  done
}

rm -rf "$work_root"
mkdir -p "$work_root/archive" "$work_root/src-rock"

check_rockspec "$rockspec"
scan_no_private_paths "$archive"
scan_no_private_paths "$src_rock"

tar -C "$work_root/archive" -xzf "$archive"
[ -d "$work_root/archive/$package_name" ] || fail "Lua source archive root mismatch" "$archive"
[ -f "$work_root/archive/$package_name/VERSION" ] || fail "Lua source archive missing VERSION" "$archive"
[ -f "$work_root/archive/$package_name/RELEASE_MANIFEST" ] || fail "Lua source archive missing RELEASE_MANIFEST" "$archive"
[ -f "$work_root/archive/$package_name/vectis.rockspec.in" ] || fail "Lua source archive missing rockspec template" "$archive"
[ -f "$work_root/archive/$package_name/lua/vectis.lua" ] || fail "Lua source archive missing vectis.lua" "$archive"
[ -f "$work_root/archive/$package_name/lua/vectis/status.lua" ] || fail "Lua source archive missing vectis.status" "$archive"
grep -Fx "$version" "$work_root/archive/$package_name/VERSION" >/dev/null ||
  fail "Lua source archive VERSION mismatch" "$archive"
grep -F "lua/vectis.lua" "$work_root/archive/$package_name/RELEASE_MANIFEST" >/dev/null ||
  fail "Lua source archive manifest missing vectis.lua" "$archive"
check_lua_module_files "$work_root/archive/$package_name" "$rockspec" "$archive"

(cd "$work_root/archive/$package_name" &&
  find . \( -path './.git/*' -o -path './build/*' -o -path './dist/*' -o -path './.luarocks/*' \)) |
while IFS= read -r forbidden; do
  fail "Lua source archive contains generated or VCS state" "$forbidden"
done

if command -v unzip >/dev/null 2>&1; then
  unzip -q "$src_rock" -d "$work_root/src-rock"
else
  (cd "$work_root/src-rock" && "${CMAKE:-cmake}" -E tar xf "$src_rock")
fi

[ -f "$work_root/src-rock/vectis-$version-1.rockspec" ] ||
  fail "source rock missing rendered rockspec" "$src_rock"
[ -f "$work_root/src-rock/$package_name.tar.gz" ] ||
  fail "source rock missing Lua source archive" "$src_rock"
check_rockspec "$work_root/src-rock/vectis-$version-1.rockspec"
scan_no_private_paths "$work_root/src-rock/$package_name.tar.gz"
tar -C "$work_root/src-rock" -xzf "$work_root/src-rock/$package_name.tar.gz"
[ -d "$work_root/src-rock/$package_name" ] ||
  fail "source rock nested archive root mismatch" "$src_rock"
[ -f "$work_root/src-rock/$package_name/lua/vectis.lua" ] ||
  fail "source rock nested archive missing vectis.lua" "$src_rock"
check_lua_module_files "$work_root/src-rock/$package_name" \
  "$work_root/src-rock/vectis-$version-1.rockspec" "$src_rock"

command -v "$luarocks" >/dev/null 2>&1 || fail "luarocks unavailable" "$src_rock"
"$luarocks" install --tree "$work_root/tree" "$src_rock" >/dev/null

echo "LuaRocks artifacts ok"
