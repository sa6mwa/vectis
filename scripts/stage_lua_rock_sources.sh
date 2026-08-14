#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
version=${VECTIS_VERSION:-$("$script_dir/release_version.sh")}
dist_dir=${VECTIS_DIST_DIR:-"$repo_root/dist"}
work_root="$repo_root/build/lua-rock-stage"
package_name="vectis-lua-$version"
stage_root="$work_root/$package_name"
archive="$dist_dir/$package_name.tar.gz"
rockspec="$dist_dir/vectis-$version-1.rockspec"
src_rock="$dist_dir/vectis-$version-1.src.rock"

safe_generated_dir() {
  path=$1
  case "$path" in
    "$repo_root"/build/*|"$repo_root"/dist) ;;
    *) printf '%s\n' "refusing to clean non-generated path: $path" >&2; exit 2 ;;
  esac
}

copy_file() {
  source=$1
  target=$2
  [ -f "$repo_root/$source" ] || {
    printf '%s\n' "missing Lua source package input: $source" >&2
    exit 2
  }
  mkdir -p "$(dirname -- "$stage_root/$target")"
  cp "$repo_root/$source" "$stage_root/$target"
}

safe_generated_dir "$work_root"
safe_generated_dir "$dist_dir"

rm -rf "$work_root"
mkdir -p "$stage_root" "$dist_dir"

copy_file LICENSE LICENSE
copy_file README.md README.md
copy_file vectis.rockspec.in vectis.rockspec.in
copy_file scripts/build_lua_rock.sh scripts/build_lua_rock.sh
copy_file scripts/render_release_rockspec.sh scripts/render_release_rockspec.sh
copy_file scripts/stage_lua_rock_sources.sh scripts/stage_lua_rock_sources.sh
copy_file scripts/test_lua_rock.sh scripts/test_lua_rock.sh
copy_file scripts/validate_luarocks.sh scripts/validate_luarocks.sh
copy_file docs/lua.md docs/lua.md
copy_file docs/lua-conventions.md docs/lua-conventions.md
copy_file docs/lua-coverage-matrix.md docs/lua-coverage-matrix.md

mkdir -p "$stage_root/lua"
cp -R "$repo_root/lua/." "$stage_root/lua/"
printf 'return "%s"\n' "$version" >"$stage_root/lua/vectis/version.lua"
printf '%s\n' "$version" >"$stage_root/VERSION"

(cd "$stage_root" && find . -type f | sed 's#^\./##' | LC_ALL=C sort) >"$stage_root/RELEASE_MANIFEST"

rm -f "$archive" "$rockspec" "$src_rock"
(cd "$work_root" && tar --sort=name --owner=0 --group=0 --numeric-owner --mtime='UTC 1970-01-01' -cf - "$package_name") | gzip -n -9 >"$archive"

"$script_dir/render_release_rockspec.sh" "$rockspec" "$version" "$package_name.tar.gz" "$package_name" >/dev/null

rock_work="$work_root/source-rock"
mkdir -p "$rock_work"
cp "$archive" "$rock_work/$(basename -- "$archive")"
cp "$rockspec" "$rock_work/$(basename -- "$rockspec")"
command -v zip >/dev/null 2>&1 || {
  printf '%s\n' "zip is required to build LuaRocks source artifacts" >&2
  exit 2
}
(cd "$rock_work" && zip -q -X "$src_rock" "$(basename -- "$rockspec")" "$(basename -- "$archive")")

"$script_dir/validate_luarocks.sh" "$dist_dir" "$version"
