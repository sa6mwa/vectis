#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
description=$("$script_dir/cpkt-toolchains.sh" discover arm64-apple-darwin) || exit 1
grep -Fxq 'status=ready' <<<"$description" || exit 1
compiler=$(sed -n 's/^cc=//p' <<<"$description")
linker=$(sed -n 's/^ld=//p' <<<"$description")
prefix=$(sed -n 's/^prefix=//p' <<<"$description")
root=$(sed -n 's/^root=//p' <<<"$description")
bin_dir="$root/bin"
[ -x "$bin_dir/$prefix-install_name_tool" ] || exit 1
compgen -G "$root/SDK/MacOSX*.sdk" >/dev/null || exit 1

probe_parent=${TMPDIR:-"$script_dir/../build/osxcross-probes"}
mkdir -p "$probe_parent"
probe_dir=$(mktemp -d "$probe_parent/vectis-osxcross.XXXXXXXX")
trap 'rm -rf -- "$probe_dir"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
printf '%s\n' 'int main(void) { return 0; }' >"$probe_dir/probe.c"
PATH="$bin_dir:$PATH" "$compiler" "--ld-path=$linker" \
  "$probe_dir/probe.c" -o "$probe_dir/probe" >/dev/null 2>&1
