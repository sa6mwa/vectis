#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

usage() {
  printf '%s\n' \
    "usage: render_release_rockspec.sh [output] [version] [source-url] [source-dir]" >&2
  exit 2
}

[ "$#" -le 4 ] || usage

version=${2:-${VECTIS_VERSION:-$("$script_dir/release_version.sh")}}
output=${1:-"$repo_root/dist/vectis-$version-1.rockspec"}
source_url=${3:-"vectis-lua-$version.tar.gz"}
source_dir=${4:-"vectis-lua-$version"}
template="$repo_root/vectis.rockspec.in"

[ -f "$template" ] || {
  printf '%s\n' "missing rockspec template: $template" >&2
  exit 2
}

case "$source_url" in
  file://*|/*)
    printf '%s\n' "release rockspec source URL must not be local: $source_url" >&2
    exit 2
    ;;
esac

mkdir -p "$(dirname -- "$output")"

escape_sed() {
  printf '%s\n' "$1" | sed 's/[\/&]/\\&/g'
}

version_escaped=$(escape_sed "$version")
source_url_escaped=$(escape_sed "$source_url")
source_dir_escaped=$(escape_sed "$source_dir")

sed \
  -e "s/@VECTIS_LUA_ROCK_VERSION@/$version_escaped/g" \
  -e "s/@VECTIS_LUA_SOURCE_URL@/$source_url_escaped/g" \
  -e "s/@VECTIS_LUA_SOURCE_DIR@/$source_dir_escaped/g" \
  "$template" >"$output"

printf '%s\n' "$output"
