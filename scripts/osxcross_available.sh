#!/usr/bin/env bash
set -eu

if [ -n "${OSXCROSS_ROOT:-}" ]; then
  osxcross_root=$OSXCROSS_ROOT
else
  osxcross_root=${HOME:?HOME is required when OSXCROSS_ROOT is not set}/.local/cross/osxcross
fi

host=${VECTIS_OSXCROSS_HOST:-arm64-apple-darwin25}
bin_dir="$osxcross_root/bin"

for tool in clang ar ranlib install_name_tool otool; do
  if [ ! -x "$bin_dir/$host-$tool" ]; then
    exit 1
  fi
done

if ! compgen -G "$osxcross_root/SDK/MacOSX*.sdk" >/dev/null; then
  exit 1
fi

probe_dir=${TMPDIR:-/tmp}
probe_c=$probe_dir/vectis-osxcross-probe-$$.c
probe_bin=$probe_dir/vectis-osxcross-probe-$$
cleanup() {
  rm -f "$probe_c" "$probe_bin"
}
trap cleanup EXIT INT TERM

cat >"$probe_c" <<'EOF'
int main(void) { return 0; }
EOF

if ! "$bin_dir/$host-clang" "$probe_c" -o "$probe_bin" >/dev/null 2>&1; then
  exit 1
fi
