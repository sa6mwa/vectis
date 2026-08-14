#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
version=${VECTIS_VERSION:-$("$script_dir/release_version.sh")}
build_root=${VECTIS_LUAROCKS_BUILD_ROOT:-"$repo_root/build/luarocks"}
tree=${VECTIS_LUAROCKS_TREE:-"$build_root/tree"}
rockspec="$build_root/vectis-$version-1.rockspec"
luarocks=${LUAROCKS:-luarocks}

usage() {
  printf '%s\n' \
    "usage: build_lua_rock.sh [cc cflags libflag obj_extension lib_extension lua_incdir]" >&2
  exit 2
}

case "$#" in
  0|6) ;;
  *) usage ;;
esac

command -v "$luarocks" >/dev/null 2>&1 || {
  printf '%s\n' "luarocks is required for make lua-rock" >&2
  exit 2
}

mkdir -p "$build_root" "$tree"
"$script_dir/render_release_rockspec.sh" "$rockspec" "$version" "vectis-lua-$version.tar.gz" "vectis-lua-$version" >/dev/null

if [ "$#" -eq 6 ]; then
  "$luarocks" make --tree "$tree" "$rockspec" \
    "CC=$1" "CFLAGS=$2" "LIBFLAG=$3" "OBJ_EXTENSION=$4" \
    "LIB_EXTENSION=$5" "LUA_INCDIR=$6"
else
  "$luarocks" make --tree "$tree" "$rockspec"
fi

printf 'VECTIS_LUAROCKS_TREE=%s\n' "$tree"
