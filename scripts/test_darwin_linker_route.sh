#!/usr/bin/env bash
set -euo pipefail

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

# Discovery-only fixture: never depend on or provision the workstation MIG.
export CPKT_TOOLCHAIN_CACHE="$work/toolchain-cache"
mig_root="$CPKT_TOOLCHAIN_CACHE/roots/host-mig-puredarwin-88753c478c97b9a08bcdb66cecc68ba5881ff3af-x86_64-linux-gnu"
mkdir -p "$mig_root/bin" "$mig_root/libexec"
touch "$mig_root/TOOLCHAIN"
for tool in bin/mig bin/mig-upstream libexec/migcom; do
  printf '#!/bin/sh\nexit 0\n' >"$mig_root/$tool"
  chmod +x "$mig_root/$tool"
done

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
    --ld-path=*) selected=${arg#--ld-path=} ;;
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

for tool in ar ranlib nm otool strip install_name_tool; do
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
set(original_path "\$ENV{PATH}")
include("${repo_root}/cmake/toolchains/arm64-apple-darwin.cmake")

set(expected_bin "${osxcross_bin}")
set(expected_ld "${osxcross_bin}/${host}-ld")
set(expected_otool "${osxcross_bin}/${host}-otool")

if(NOT "\$ENV{PATH}" STREQUAL original_path)
  message(FATAL_ERROR "Discovery-only CMake unexpectedly changed PATH")
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
  string(FIND "\${\${flag_var}}" "--ld-path=\${expected_ld}" fuse_index)
  if(fuse_index LESS 0)
    message(FATAL_ERROR "Darwin toolchain did not force target linker in \${flag_var}: \${\${flag_var}}")
  endif()
endforeach()
separate_arguments(link_flags NATIVE_COMMAND "\${CMAKE_EXE_LINKER_FLAGS}")
execute_process(COMMAND "\${CMAKE_C_COMPILER}" \${link_flags}
  "${work}/main.c" -o "${work}/fixed" RESULT_VARIABLE linked)
if(NOT linked EQUAL 0)
  message(FATAL_ERROR "Selected compiler/linker invocation failed")
endif()
EOF

OSXCROSS_ROOT="$osxcross_root" CPKT_OSXCROSS_HOST="$host" \
  VECTIS_LINKER_ROUTE_LOG="$log" PATH="$host_bin:$PATH" \
  "${CMAKE:-cmake}" -P "$work/check-toolchain.cmake"
fixed_route=$(tail -n 1 "$log")
if [ "$fixed_route" != "$osxcross_bin/$host-ld" ]; then
  echo "CMake flags did not force the target Darwin linker: $fixed_route" >&2
  exit 1
fi

if ! OSXCROSS_ROOT="$osxcross_root" CPKT_OSXCROSS_HOST="$host" \
    VECTIS_LINKER_ROUTE_LOG="$log" TMPDIR="$work" \
    PATH="$host_bin:$osxcross_bin:$PATH" \
    bash "$repo_root/scripts/osxcross_available.sh"; then
  echo "complete synthetic osxcross toolchain was not detected" >&2
  exit 1
fi
mv "$osxcross_bin/$host-nm" "$osxcross_bin/$host-nm.missing"
if OSXCROSS_ROOT="$osxcross_root" CPKT_OSXCROSS_HOST="$host" \
    VECTIS_LINKER_ROUTE_LOG="$log" TMPDIR="$work" \
    PATH="$host_bin:$osxcross_bin:$PATH" \
    bash "$repo_root/scripts/osxcross_available.sh"; then
  echo "incomplete synthetic osxcross toolchain was accepted" >&2
  exit 1
fi
mv "$osxcross_bin/$host-nm.missing" "$osxcross_bin/$host-nm"

echo "darwin linker route ok"
