#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/.." && pwd)
package_root=${1:?usage: verify_installed_sdk.sh PACKAGE_ROOT [static|shared]}
link_mode=${2:-static}
extra_prefix=${3:-}
cmake_bin=${CMAKE:-cmake}
mkdir -p "$repo_root/build/sdk-verification"
work_root=$(mktemp -d "$repo_root/build/sdk-verification/run.XXXXXXXX")
build_root=$work_root/consumer
examples_build_root=$work_root/examples
pkg_config_build_root=$work_root/pkg-config

cleanup() {
  rm -rf "$build_root"
  rm -rf "$examples_build_root"
  rm -rf "$pkg_config_build_root"
  rmdir "$work_root"
}
trap cleanup EXIT INT TERM

package_root=$(CDPATH='' cd -- "$package_root" && pwd)
if [ -n "$extra_prefix" ]; then
  extra_prefix=$(CDPATH='' cd -- "$extra_prefix" && pwd)
fi
cmake_prefix_path="$package_root"
if [ -n "$extra_prefix" ]; then
  cmake_prefix_path="$cmake_prefix_path;$extra_prefix"
fi
runtime_library_path="$package_root/lib"
if [ -n "$extra_prefix" ]; then
  runtime_library_path="$runtime_library_path:$extra_prefix/lib"
fi

toolchain_args=()
runtime_flags=()
consumer_cc=${CC:-cc}
if [ "$(uname -s)" = Linux ]; then
  if [ "$(uname -m)" != x86_64 ]; then
    echo 'native SDK runtime verification currently requires x86_64 Linux' >&2
    exit 2
  fi
  description=$("$script_dir/cpkt-toolchains.sh" discover x86_64-linux-gnu)
  consumer_cc=$(printf '%s\n' "$description" | sed -n 's/^cc=//p')
  consumer_sysroot=$(printf '%s\n' "$description" | sed -n 's/^sysroot=//p')
  [ -x "$consumer_cc" ] && [ -f "$consumer_sysroot/lib/libc.so.6" ] || {
    echo 'Pinned Bootlin runtime is unavailable; run make build first' >&2
    exit 2
  }
  toolchain_args=(
    "-DCMAKE_TOOLCHAIN_FILE=$repo_root/cmake/toolchains/x86_64-linux-gnu.cmake"
    "-DVECTIS_TARGET_ID=x86_64-linux-gnu"
    "-DVECTIS_EXTERNAL_ROOT=$package_root"
    "-DCMAKE_PROJECT_INCLUDE=$repo_root/cmake/test_runtime.cmake"
  )
  runtime_flags=("-Wl,--dynamic-linker=$consumer_sysroot/lib/ld-linux-x86-64.so.2"
    '-Wl,--disable-new-dtags'
    "-Wl,-rpath,$consumer_sysroot/lib:$consumer_sysroot/usr/lib:$runtime_library_path")
  unset LD_LIBRARY_PATH LD_PRELOAD LD_AUDIT
fi

case "$link_mode" in
  static|shared) ;;
  *)
    echo "link mode must be static or shared" >&2
    exit 2
    ;;
esac

"$cmake_bin" \
  "${toolchain_args[@]}" \
  -S "$repo_root/tests/install" \
  -B "$build_root" \
  -DCMAKE_PREFIX_PATH="$cmake_prefix_path" \
  -DVECTIS_CONSUMER_LINK="$link_mode"
"$cmake_bin" --build "$build_root"
if [ "$(uname -s)" = Linux ]; then
  python3 "$script_dir/test_runtime_contract.py" --sdk "$build_root"
fi

if command -v pkg-config >/dev/null 2>&1; then
  mkdir -p "$pkg_config_build_root"
  cat >"$pkg_config_build_root/consumer.c" <<'EOF'
#include <vectis/vectis.h>

int main(void) {
  vectis_app_config config;
  vectis_app_config_init(&config);
  return config.tls.port == 8443u ? 0 : 1;
}
EOF
  PKG_CONFIG_PATH="$package_root/lib/pkgconfig${extra_prefix:+:$extra_prefix/lib/pkgconfig}" \
    pkg-config --exists vectis
  PKG_CONFIG_PATH="$package_root/lib/pkgconfig${extra_prefix:+:$extra_prefix/lib/pkgconfig}" \
    pkg-config --static --cflags --libs vectis >"$pkg_config_build_root/vectis.pc.flags"
  read -r -a pkg_flags <"$pkg_config_build_root/vectis.pc.flags"
  "$consumer_cc" "${runtime_flags[@]}" "$pkg_config_build_root/consumer.c" \
    -o "$pkg_config_build_root/vectis_pkg_config_consumer" \
    "${pkg_flags[@]}"
  "$pkg_config_build_root/vectis_pkg_config_consumer"
fi

if [ "$link_mode" = "shared" ]; then
  if [ "$(uname -s)" = "Darwin" ]; then
    DYLD_LIBRARY_PATH="$runtime_library_path${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
      "$build_root/vectis_install_consumer"
    if [ -x "$build_root/vectis_install_consumer_cpp" ]; then
      DYLD_LIBRARY_PATH="$runtime_library_path${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
        "$build_root/vectis_install_consumer_cpp"
    fi
  else
    LD_LIBRARY_PATH="$runtime_library_path${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
      "$build_root/vectis_install_consumer"
    if [ -x "$build_root/vectis_install_consumer_cpp" ]; then
      LD_LIBRARY_PATH="$runtime_library_path${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$build_root/vectis_install_consumer_cpp"
    fi
  fi
else
  "$build_root/vectis_install_consumer"
  if [ -x "$build_root/vectis_install_consumer_cpp" ]; then
    "$build_root/vectis_install_consumer_cpp"
  fi
fi

if [ -f "$package_root/share/doc/vectis/examples/CMakeLists.txt" ]; then
  "$cmake_bin" \
    "${toolchain_args[@]}" \
    -S "$package_root/share/doc/vectis/examples" \
    -B "$examples_build_root" \
    -DCMAKE_PREFIX_PATH="$cmake_prefix_path" \
    -DVECTIS_EXAMPLE_LINK="$link_mode"
  "$cmake_bin" --build "$examples_build_root"
  if [ "$(uname -s)" = Linux ]; then
    python3 "$script_dir/test_runtime_contract.py" --sdk "$examples_build_root"
  fi
fi
