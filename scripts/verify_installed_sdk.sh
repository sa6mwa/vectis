#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
package_root=${1:?usage: verify_installed_sdk.sh PACKAGE_ROOT [static|shared]}
link_mode=${2:-static}
extra_prefix=${3:-}
cmake_bin=${CMAKE:-cmake}
build_root=${TMPDIR:-/tmp}/vectis-install-sdk-${link_mode}-$$
examples_build_root=${TMPDIR:-/tmp}/vectis-install-examples-${link_mode}-$$
pkg_config_build_root=${TMPDIR:-/tmp}/vectis-install-pkg-config-${link_mode}-$$

package_root=$(CDPATH= cd -- "$package_root" && pwd)
if [ -n "$extra_prefix" ]; then
  extra_prefix=$(CDPATH= cd -- "$extra_prefix" && pwd)
fi
cmake_prefix_path="$package_root"
if [ -n "$extra_prefix" ]; then
  cmake_prefix_path="$cmake_prefix_path;$extra_prefix"
fi
runtime_library_path="$package_root/lib"
if [ -n "$extra_prefix" ]; then
  runtime_library_path="$runtime_library_path:$extra_prefix/lib"
fi

case "$link_mode" in
  static|shared) ;;
  *)
    echo "link mode must be static or shared" >&2
    exit 2
    ;;
esac

cleanup() {
  rm -rf "$build_root"
  rm -rf "$examples_build_root"
  rm -rf "$pkg_config_build_root"
}
trap cleanup EXIT INT TERM

"$cmake_bin" \
  -S "$repo_root/tests/install" \
  -B "$build_root" \
  -DCMAKE_PREFIX_PATH="$cmake_prefix_path" \
  -DVECTIS_CONSUMER_LINK="$link_mode"
"$cmake_bin" --build "$build_root"

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
  cc "$pkg_config_build_root/consumer.c" \
    -o "$pkg_config_build_root/vectis_pkg_config_consumer" \
    $(PKG_CONFIG_PATH="$package_root/lib/pkgconfig${extra_prefix:+:$extra_prefix/lib/pkgconfig}" \
        pkg-config --static --cflags --libs vectis)
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
    -S "$package_root/share/doc/vectis/examples" \
    -B "$examples_build_root" \
    -DCMAKE_PREFIX_PATH="$cmake_prefix_path" \
    -DVECTIS_EXAMPLE_LINK="$link_mode"
  "$cmake_bin" --build "$examples_build_root"
fi
