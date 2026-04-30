#!/usr/bin/env bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
package_root=${1:?usage: verify_installed_sdk.sh PACKAGE_ROOT [static|shared]}
link_mode=${2:-static}
cmake_bin=${CMAKE:-cmake}
build_root=${TMPDIR:-/tmp}/vectis-install-sdk-${link_mode}-$$
examples_build_root=${TMPDIR:-/tmp}/vectis-install-examples-${link_mode}-$$

package_root=$(CDPATH= cd -- "$package_root" && pwd)

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
}
trap cleanup EXIT INT TERM

"$cmake_bin" \
  -S "$repo_root/tests/install" \
  -B "$build_root" \
  -DCMAKE_PREFIX_PATH="$package_root" \
  -DVECTIS_CONSUMER_LINK="$link_mode"
"$cmake_bin" --build "$build_root"

if [ "$link_mode" = "shared" ]; then
  if [ "$(uname -s)" = "Darwin" ]; then
    DYLD_LIBRARY_PATH="$package_root/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
      "$build_root/vectis_install_consumer"
    if [ -x "$build_root/vectis_install_consumer_cpp" ]; then
      DYLD_LIBRARY_PATH="$package_root/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
        "$build_root/vectis_install_consumer_cpp"
    fi
  else
    LD_LIBRARY_PATH="$package_root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
      "$build_root/vectis_install_consumer"
    if [ -x "$build_root/vectis_install_consumer_cpp" ]; then
      LD_LIBRARY_PATH="$package_root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
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
    -DCMAKE_PREFIX_PATH="$package_root" \
    -DVECTIS_EXAMPLE_LINK="$link_mode"
  "$cmake_bin" --build "$examples_build_root"
fi
