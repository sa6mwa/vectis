#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
work="$repo_root/build/test-darwin-linker-route"
host=${CPKT_OSXCROSS_HOST:-arm64-apple-darwin25}
osxcross_root="$work/osxcross"
osxcross_bin="$osxcross_root/bin"
host_bin="$work/hostbin"
log="$work/linker-route.log"

rm -rf "$work"
mkdir -p "$osxcross_bin" "$host_bin" "$osxcross_root/SDK/MacOSX15.sdk/usr/include"

cleanup() {
  rm -rf "$work"
}
trap cleanup EXIT INT TERM

cat >"$host_bin/ld" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$host_bin/ld"

cat >"$osxcross_bin/$host-ld" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$osxcross_bin/$host-ld"

cat >"$osxcross_bin/$host-clang" <<'EOF'
#!/bin/sh
log=${VECTIS_LINKER_ROUTE_LOG:?VECTIS_LINKER_ROUTE_LOG is required}
selected=
out=
want_out=0

for arg in "$@"; do
  if [ "$want_out" = 1 ]; then
    out=$arg
    want_out=0
    continue
  fi
  case "$arg" in
    -fuse-ld=*) selected=${arg#-fuse-ld=} ;;
    -o) want_out=1 ;;
  esac
done

if [ -z "$selected" ]; then
  selected=$(command -v ld 2>/dev/null || true)
fi
printf '%s\n' "$selected" >>"$log"
[ -n "$out" ] && : >"$out"
exit 0
EOF
chmod +x "$osxcross_bin/$host-clang"
cp "$osxcross_bin/$host-clang" "$osxcross_bin/$host-clang++"

for tool in ar ranlib otool strip install_name_tool; do
  cat >"$osxcross_bin/$host-$tool" <<'EOF'
#!/bin/sh
out=
want_out=0
for arg in "$@"; do
  if [ "$want_out" = 1 ]; then
    out=$arg
    want_out=0
    continue
  fi
  [ "$arg" = "-o" ] && want_out=1
done
[ -n "$out" ] && : >"$out"
exit 0
EOF
  chmod +x "$osxcross_bin/$host-$tool"
done

printf '%s\n' 'int main(void) { return 0; }' >"$work/main.c"

: >"$log"
PATH="$host_bin:$osxcross_bin:$PATH" VECTIS_LINKER_ROUTE_LOG="$log" \
  "$osxcross_bin/$host-clang" "$work/main.c" -o "$work/unfixed"
unfixed_route=$(sed -n '1p' "$log")
if [ "$unfixed_route" != "$host_bin/ld" ]; then
  echo "fake osxcross wrapper did not demonstrate host linker selection without lifecycle route" >&2
  echo "expected: $host_bin/ld" >&2
  echo "actual:   $unfixed_route" >&2
  exit 1
fi

cat >"$work/check-toolchain.cmake" <<EOF
include("${repo_root}/cmake/toolchains/arm64-apple-darwin.cmake")

set(expected_bin "${osxcross_bin}")
set(expected_ld "${osxcross_bin}/${host}-ld")
set(expected_otool "${osxcross_bin}/${host}-otool")

string(FIND "\$ENV{PATH}" "\${expected_bin}:" path_index)
if(NOT path_index EQUAL 0)
  message(FATAL_ERROR "Darwin toolchain did not prepend osxcross bin to PATH: \$ENV{PATH}")
endif()

if(NOT CMAKE_LINKER STREQUAL "\${expected_ld}")
  message(FATAL_ERROR "Darwin toolchain selected wrong linker: \${CMAKE_LINKER}")
endif()

foreach(otool_var VECTIS_OTOOL CMAKE_OTOOL CPKT_OTOOL _cai_otool)
  if(NOT "\${\${otool_var}}" STREQUAL "\${expected_otool}")
    message(FATAL_ERROR "Darwin toolchain selected wrong Mach-O inspector in \${otool_var}: \${\${otool_var}}")
  endif()
endforeach()

foreach(flag_var CMAKE_EXE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS CMAKE_MODULE_LINKER_FLAGS)
  string(FIND "\${\${flag_var}}" "-fuse-ld=\${expected_ld}" fuse_index)
  if(fuse_index LESS 0)
    message(FATAL_ERROR "Darwin toolchain did not force target linker in \${flag_var}: \${\${flag_var}}")
  endif()
endforeach()
EOF

OSXCROSS_ROOT="$osxcross_root" CPKT_OSXCROSS_HOST="$host" \
  "${CMAKE:-cmake}" -P "$work/check-toolchain.cmake"

: >"$log"
PATH="$osxcross_bin:$host_bin:$PATH" VECTIS_LINKER_ROUTE_LOG="$log" \
  "$osxcross_bin/$host-clang" "-fuse-ld=$osxcross_bin/$host-ld" "$work/main.c" -o "$work/fixed"
fixed_route=$(sed -n '1p' "$log")
if [ "$fixed_route" != "$osxcross_bin/$host-ld" ]; then
  echo "lifecycle route did not force the target Darwin linker" >&2
  echo "expected: $osxcross_bin/$host-ld" >&2
  echo "actual:   $fixed_route" >&2
  exit 1
fi

echo "darwin linker route ok"
