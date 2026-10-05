#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
preset=${1:-deps-host-debug}

dependency_cache="${CPKT_DEPENDENCY_CACHE:-${XDG_CACHE_HOME:-${HOME}/.cache}/c.pkt.systems/deps}"
archive_cache_root="$dependency_cache/archives/sha256"
archive_lock_root="$dependency_cache/locks"
deps_root=
target_id=
system_sha256=
lockdc_sha256=
lonejson_sha256=
pslog_sha256=
cai_sha256=
lql_sha256=
lql_lua_sha256="c4610615f4315081c519afe47d519e72c1242cce449620cf90f8f8828eccdfe1"
mdf_sha256=
softline_sha256=
target_cc=
target_ar=
target_ranlib=
target_cmake_system_name=
target_cmake_system_processor=
pid0_enabled=0

set_linux_gnu_dependency_target() {
  target_id=$1
  case "$target_id" in
    x86_64-linux-gnu)
      system_sha256="fbd3e3f357c22dc26b94cc7e5e200651b59ecb39357ce7bb8c4cb3d939a2b557"
      lockdc_sha256="1e949887d613824716645dd5e9a85a1b89ad2d8d66431838c0a28c999b0e23da"
      lonejson_sha256="aef11c4511bb6aa8ed4e0646204e14b7934affd6e5cec5b8e8bff36e80217826"
      pslog_sha256="21239262bc06eecb504e45e0902f374ccd86751dcb32087dd84af4d2553ea0e2"
      cai_sha256="f7f78d9f847f1265267bbc33a6d6cb2058602d82967357ab561f36ebb1d0daf8"
      lql_sha256="fafb04f9af5d9610f77380ac1f848d1edd3d60dcf7ce6a098f6a04e30dd2b47a"
      mdf_sha256="b250a82e2562897102aee69bca2c946e53eadffddd1d85131975eaea00651b65"
      softline_sha256="09612ec2cc3fc1ecdcefaf4afff4e5e80555d9618c13a7e6db3405d0ead835f5"
      target_cmake_system_processor="x86_64"
      ;;
    aarch64-linux-gnu)
      system_sha256="940c3d63f0d9b436cc3f27b73547b62e989fae258915274749160446e936124a"
      lockdc_sha256="7ccc2d6789a8948a7d43041b5db17c6a107ff1e2701c4b43980bbb0c6ff79e11"
      lonejson_sha256="659cc67e2c26c9615f84fa6f1c26fae4a3a4b00980d8bbb02c04625b2facaf7e"
      pslog_sha256="304575fa07f14e5ff4d7614f7c450a3e88a586d5f353352bff8b678ee739da76"
      cai_sha256="4fa244a597378ee097a10ee5a472c8d07df5208e6a570b6756ccae40e96abb18"
      lql_sha256="becc67d67c9e50291d1cc0c7bfc124c7945576b773013b37f65e969bb32af4a8"
      mdf_sha256="2909a4888a8e422ba0fe59efd6676a105be692fce5f67d0b2d0d3ea3e978afb6"
      softline_sha256="5c6b6994b4bb4ccb030927e5e5d46cccea3e8bfe9188fb89f1cf7321dddcec46"
      target_cmake_system_processor="aarch64"
      ;;
    armhf-linux-gnu)
      system_sha256="ca71287d539f550ef97311b12637a32e76ef163e4ac70edcfa26831474f41677"
      lockdc_sha256="e1a028d02637920a1a3d710b8fa81f23c78e85450ebed252feb131ab0f62084a"
      lonejson_sha256="ab2f89eb4054fbb1b4cd85b6be5d9d2005b364459ba362b910bc35ac1c6511b5"
      pslog_sha256="993461d481ec6c3912be29f5a0f95bcf5274aae6a1d2d698350e804c688122a9"
      cai_sha256="f98666e5b0eb60d6a61bfa0f8ca10b018e4bc32a267cd7fc2a1532d9e256a7dd"
      lql_sha256="3a022733ffa7ba808127d04669a6e091b2bdfcf39c9fd3092b9d0769d481db82"
      mdf_sha256="2534b31a2c6222fac57b30e6ed3f63424d6b981c8bf1d5ad54fdebb00827b136"
      softline_sha256="415f0934756f2079b5aeffa4aabb98fd09d15b864224673f63c5e297ad08cb87"
      target_cmake_system_processor="arm"
      ;;
    *)
      echo "unsupported Linux GNU dependency target: $target_id" >&2
      exit 2
      ;;
  esac
  target_cc="cpkt-toolchains:$target_id"
  target_ar="cpkt-toolchains:$target_id"
  target_ranlib="cpkt-toolchains:$target_id"
  target_cmake_system_name="Linux"
}

set_host_debug_target() {
  host_system=${VECTIS_HOST_UNAME_S:-$(uname -s)}
  host_machine=${VECTIS_HOST_UNAME_M:-$(uname -m)}

  case "$host_system:$host_machine" in
    Linux:x86_64 | Linux:amd64)
      set_linux_gnu_dependency_target "x86_64-linux-gnu"
      ;;
    Linux:aarch64 | Linux:arm64)
      set_linux_gnu_dependency_target "aarch64-linux-gnu"
      ;;
    Linux:armv6l | Linux:armv7l | Linux:armv8l)
      set_linux_gnu_dependency_target "armhf-linux-gnu"
      ;;
    *)
      echo "unsupported host for deps-host-debug: $host_system $host_machine" >&2
      echo "supported hosts: Linux x86_64, Linux aarch64, Linux armv6l/armv7l/armv8l" >&2
      exit 2
      ;;
  esac
}

case "$preset" in
  deps-host-debug)
    deps_root="$repo_root/.cache/deps/host-debug"
    set_host_debug_target
    ;;
  deps-x86_64-linux-gnu)
    deps_root="$repo_root/.cache/deps/x86_64-linux-gnu"
    set_linux_gnu_dependency_target "x86_64-linux-gnu"
    ;;
  deps-x86_64-linux-musl)
    deps_root="$repo_root/.cache/deps/x86_64-linux-musl"
    target_id="x86_64-linux-musl"
    system_sha256="f07a729e860efb275874871d73f3fcdde5ab6e0395762963f90b9db481ae34b8"
    lockdc_sha256="580b8f4b8dd87d77a3ef16f8a304ce3dc472e45e64e39ec1250a8582a8182c31"
    lonejson_sha256="823efad9719011af0602cefacf0506ac603bfb54d5986883fcabed60db93544b"
    pslog_sha256="3a43206260bbf1095a363ffc4ddb5514dac2a621aa030db0e7f9b4364a93bb4d"
    cai_sha256="5a9a857137e1fa8fd1679fe8d6c6b0d84f69bdd905deeb2fdcbf35db1663872c"
    lql_sha256="e33a57e5d97337304c8541efaf8ab33bff9060cae600d1fe744a6734dca74ad4"
    mdf_sha256="144827811d9e1477a6e0835b8a94492c2a1ba7eb7fa6d091ee0e0c73f2e7b2ee"
    softline_sha256="4779f8950fc4abf29ea87dae2ab31e7a951ee760d3d62bce0b4ef199c6625e49"
    target_cc="cpkt-toolchains:$target_id"
    target_ar="cpkt-toolchains:$target_id"
    target_ranlib="cpkt-toolchains:$target_id"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="x86_64"
    ;;
  deps-aarch64-linux-gnu)
    deps_root="$repo_root/.cache/deps/aarch64-linux-gnu"
    set_linux_gnu_dependency_target "aarch64-linux-gnu"
    target_cc="cpkt-toolchains:$target_id"
    target_ar="cpkt-toolchains:$target_id"
    target_ranlib="cpkt-toolchains:$target_id"
    ;;
  deps-aarch64-linux-musl)
    deps_root="$repo_root/.cache/deps/aarch64-linux-musl"
    target_id="aarch64-linux-musl"
    system_sha256="6de39175f8f399a966d0aa925fcf665dd2c221bc38255624215f2faf332ab248"
    lockdc_sha256="62aade3ef46dfc57a2e49c4467dc320aaff0aa4c7afab1ebe76596eba970ebcc"
    lonejson_sha256="96e020ce3dab9de5dc13a64e83a278b7519cdf7fcc2d3c93fdfc740fe5fb0cd5"
    pslog_sha256="72a3b4e1f89c339cadb996137a79db8ee6e67be1f580f482269f6b0088cda115"
    cai_sha256="0682edace79a151bde43b040933d10d2ead4659aba02bc1d7daf0e06b36c4861"
    lql_sha256="8981ed9a6d938d1fdcb9da08ebc30df5d48d78bcad423bd1c64e858795d17ccf"
    mdf_sha256="0f6ebaf7dd8754fa08bef3b14b7a15dc8f5074bf64ecd353d39e007aa963a574"
    softline_sha256="1b81acea3047801172857e6df1b7e3197a9fc116b88a4bf36e7422e88de7e22f"
    target_cc="cpkt-toolchains:$target_id"
    target_ar="cpkt-toolchains:$target_id"
    target_ranlib="cpkt-toolchains:$target_id"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="aarch64"
    ;;
  deps-armhf-linux-gnu)
    deps_root="$repo_root/.cache/deps/armhf-linux-gnu"
    set_linux_gnu_dependency_target "armhf-linux-gnu"
    target_cc="cpkt-toolchains:$target_id"
    target_ar="cpkt-toolchains:$target_id"
    target_ranlib="cpkt-toolchains:$target_id"
    ;;
  deps-armhf-linux-musl)
    deps_root="$repo_root/.cache/deps/armhf-linux-musl"
    target_id="armhf-linux-musl"
    system_sha256="4c4eabad12354164f3fbc8f181e99e49d34fa073be9a4a328070493f27f1374e"
    lockdc_sha256="75870967e2260c1256220e6ac274346d7ffa97f2c8e3acf1e367d60f514143d7"
    lonejson_sha256="8b103a732181b06d90fc5f39c074a887969522d29e0d9006b30982f31969982d"
    pslog_sha256="037fa1d56a01dc8b9fccf54960b8083337cbc5edfaa5f15d715f96413d1f6b77"
    cai_sha256="90e8982c3accf4accc338e4c6ec9a53b1d80d6f62e4d6d50f5c32de4599d9f52"
    lql_sha256="0eddb49271b137e1b5094d8954c7a16ef66f8a8ae009476402272a7e10d5b001"
    mdf_sha256="1b02a34681d4e730849b8bd90afe52c44f0d825436e52674aa45c70f5e2bdc5f"
    softline_sha256="33ce2ad7b5c90b6ab3ac63efb4512dc107023389b580290cc05117aa827703ad"
    target_cc="cpkt-toolchains:$target_id"
    target_ar="cpkt-toolchains:$target_id"
    target_ranlib="cpkt-toolchains:$target_id"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="arm"
    ;;
  deps-arm64-apple-darwin)
    deps_root="$repo_root/.cache/deps/arm64-apple-darwin"
    target_id="arm64-apple-darwin"
    system_sha256="89e6d81df7a41d076a6067d6a79545e9ed318bef595a506e1da02b1b6fd601df"
    lockdc_sha256="b2317c309c03e6eea1f40ce89ad31570c95f2f7dd3538ae1347109e52b53a251"
    lonejson_sha256="9dc6b277241068a2ab1a737357cf18982c5612ec43280f161044ce719aed50dc"
    pslog_sha256="a5945f2dc4ad16dd8a71572f40117ddfdfae371d185cb03dab5205cdf780c321"
    cai_sha256="1921498c951abd7aa093760f3307563a9ac6e98f22fffc8ec7f81d7649de47b6"
    lql_sha256="24b60e9f73d4341451aeb43e331bb29569f112d0b940130dbf9eb401b054408c"
    mdf_sha256="d4cf9445fe175810a2cd223a7928374094aab2944894a99b50b3b11cac8972fe"
    softline_sha256="93741baf56667c441438af521354db8178135b654e944eb4e49a276ddbb8282d"
    if [ -n "${OSXCROSS_ROOT:-}" ]; then
      osxcross_root=$OSXCROSS_ROOT
    else
      osxcross_root="${HOME:-}/.local/cross/osxcross"
    fi
    osxcross_host="${VECTIS_OSXCROSS_HOST:-${CPKT_OSXCROSS_HOST:-arm64-apple-darwin25}}"
    target_cc="${CC:-$osxcross_root/bin/$osxcross_host-clang}"
    target_ar="${AR:-$osxcross_root/bin/$osxcross_host-ar}"
    target_ranlib="${RANLIB:-$osxcross_root/bin/$osxcross_host-ranlib}"
    target_cmake_system_name="Darwin"
    target_cmake_system_processor="arm64"
    ;;
  *)
    echo "usage: scripts/deps.sh [deps-host-debug|deps-x86_64-linux-gnu|deps-x86_64-linux-musl|deps-aarch64-linux-gnu|deps-aarch64-linux-musl|deps-armhf-linux-gnu|deps-armhf-linux-musl|deps-arm64-apple-darwin]" >&2
    exit 2
    ;;
esac

case "$target_cmake_system_name" in
  Linux) pid0_enabled=1 ;;
  *) pid0_enabled=0 ;;
esac

if [ "${VECTIS_DEPS_DRY_RUN:-0}" = "1" ]; then
  cat <<EOF
preset=$preset
dependency_cache=$dependency_cache
deps_root=$deps_root
target_id=$target_id
target_cc=$target_cc
target_ar=$target_ar
target_ranlib=$target_ranlib
target_cmake_system_name=$target_cmake_system_name
target_cmake_system_processor=$target_cmake_system_processor
system_sha256=$system_sha256
liblockdc_sha256=$lockdc_sha256
lonejson_sha256=$lonejson_sha256
pslog_sha256=$pslog_sha256
cai_sha256=$cai_sha256
lql_sha256=$lql_sha256
lql_lua_sha256=$lql_lua_sha256
mdf_sha256=$mdf_sha256
softline_sha256=$softline_sha256
libpid0_enabled=$pid0_enabled
EOF
  if [ "$pid0_enabled" -eq 1 ]; then
    cat <<EOF
libpid0_version=0.5.0
libpid0_sha256=9647a9027e3f5086278cc628b4cfdf853e239a04c202b4f3577691202431c7e1
EOF
  fi
  exit 0
fi

patch_lockdc_lua_source() {
  file="$lockdc_lua_source_dir/src/lua/lockdc_lua.c"
  tmp="$file.tmp.$$"

  if [ ! -f "$file" ]; then
    echo "missing staged lockdc Lua source: $file" >&2
    exit 1
  fi
  if grep -Fq 'lcdc_opt_version_field' "$file"; then
    return 0
  fi
  if ! grep -Fq 'static int lcdc_opt_integer_field(lua_State *L, int index, const char *name,' "$file"; then
    echo "unsupported lockdc Lua source layout: missing integer option helper" >&2
    exit 1
  fi

  awk '
    {
      print
      if ($0 == "static int lcdc_opt_integer_field(lua_State *L, int index, const char *name,") {
        in_integer_helper = 1
      } else if (in_integer_helper && $0 == "}") {
        print ""
        print "static int lcdc_opt_version_field(lua_State *L, int index, const char *name,"
        print "                                  lc_version *out) {"
        print "  if (lua_istable(L, index)) {"
        print "    lua_getfield(L, index, name);"
        print "    if (!lua_isnil(L, -1)) {"
        print "      *out = (lc_version)luaL_checkinteger(L, -1);"
        print "      lua_pop(L, 1);"
        print "      return 1;"
        print "    }"
        print "    lua_pop(L, 1);"
        print "  }"
        print "  return 0;"
        print "}"
        in_integer_helper = 0
      }
    }
  ' "$file" >"$tmp"
  mv "$tmp" "$file"
  sed -i \
    's/lcdc_opt_integer_field(L, \([^,][^,]*\), "if_version"/lcdc_opt_version_field(L, \1, "if_version"/g' \
    "$file"
  if ! grep -Fq 'lcdc_opt_version_field(lua_State *L, int index, const char *name,' "$file"; then
    echo "failed to patch lockdc Lua version helper" >&2
    exit 1
  fi
}

mkdir -p "$archive_cache_root" "$archive_lock_root" "$deps_root/include" "$deps_root/lib"

system_version="0.11.0"
system_archive="c.pkt.systems-${system_version}-${target_id}.tar.gz"
system_url="https://github.com/sa6mwa/c.pkt.systems/releases/download/v${system_version}/${system_archive}"
system_download="$archive_cache_root/$system_sha256/$system_archive"
lockdc_version="0.18.0"
lockdc_archive="liblockdc-${lockdc_version}-${target_id}.tar.gz"
lockdc_url="https://github.com/sa6mwa/liblockdc/releases/download/v${lockdc_version}/${lockdc_archive}"
lockdc_download="$archive_cache_root/$lockdc_sha256/$lockdc_archive"
lockdc_lua_archive="liblockdc-lua-${lockdc_version}.tar.gz"
lockdc_lua_payload=""
lockdc_lua_url="https://github.com/sa6mwa/liblockdc/releases/download/v${lockdc_version}/${lockdc_lua_archive}"
lockdc_lua_sha256="94852adfc713299d748d808cd4ff5cbe3e4d66f0bc6ce5be6e80d97cd981cc89"
lockdc_lua_download="$archive_cache_root/$lockdc_lua_sha256/$lockdc_lua_archive"
lockdc_lua_source_dir="$deps_root/share/lockdc-source"
lonejson_version="0.45.0"
lonejson_archive="liblonejson-${lonejson_version}-${target_id}.tar.gz"
lonejson_url="https://github.com/sa6mwa/lonejson/releases/download/v${lonejson_version}/${lonejson_archive}"
lonejson_download="$archive_cache_root/$lonejson_sha256/$lonejson_archive"
lonejson_lua_archive="lonejson-lua-${lonejson_version}.tar.gz"
lonejson_lua_payload=""
lonejson_lua_url="https://github.com/sa6mwa/lonejson/releases/download/v${lonejson_version}/${lonejson_lua_archive}"
lonejson_lua_sha256="1066e37671f0f1e4076aa7f71206a0e29220cafb1508526d510abca0ab9dfb7d"
lonejson_lua_download="$archive_cache_root/$lonejson_lua_sha256/$lonejson_lua_archive"
lonejson_source_dir="$deps_root/share/lonejson-source"
pslog_version="0.12.0"
pslog_archive="libpslog-${pslog_version}-${target_id}.tar.gz"
pslog_url="https://github.com/sa6mwa/libpslog/releases/download/v${pslog_version}/${pslog_archive}"
pslog_download="$archive_cache_root/$pslog_sha256/$pslog_archive"
pslog_lua_archive="lua-pslog-${pslog_version}.tar.gz"
pslog_lua_url="https://github.com/sa6mwa/libpslog/releases/download/v${pslog_version}/${pslog_lua_archive}"
pslog_lua_sha256="35e46f594b2ce12b42bba95357109fd662ce1c2ab7a175702e9b9d141cb0ec2b"
pslog_lua_download="$archive_cache_root/$pslog_lua_sha256/$pslog_lua_archive"
pslog_lua_source_dir="$deps_root/share/pslog-lua-source"
cai_version="0.6.0"
cai_archive="cai-${cai_version}-${target_id}.tar.gz"
cai_url="https://github.com/sa6mwa/cai/releases/download/v${cai_version}/${cai_archive}"
cai_download="$archive_cache_root/$cai_sha256/$cai_archive"
cai_lua_archive="cai-lua-${cai_version}.tar.gz"
cai_lua_payload=""
cai_lua_url="https://github.com/sa6mwa/cai/releases/download/v${cai_version}/${cai_lua_archive}"
cai_lua_sha256="977e9b615dd78127fba6574eb2ac8527f52c4e6c578faeece15f39c8ed7eaeab"
cai_lua_download="$archive_cache_root/$cai_lua_sha256/$cai_lua_archive"
cai_lua_source_dir="$deps_root/share/cai-lua-source"
lql_version="0.3.0"
lql_archive="liblql-${lql_version}-${target_id}.tar.gz"
lql_url="https://github.com/sa6mwa/liblql/releases/download/v${lql_version}/${lql_archive}"
lql_download="$archive_cache_root/$lql_sha256/$lql_archive"
lql_lua_archive="liblql-lua-${lql_version}.tar.gz"
lql_lua_url="https://github.com/sa6mwa/liblql/releases/download/v${lql_version}/${lql_lua_archive}"
lql_lua_download="$archive_cache_root/$lql_lua_sha256/$lql_lua_archive"
lql_lua_source_dir="$deps_root/share/liblql-lua-source"
mdf_version="0.14.0"
mdf_archive="libmdf-${mdf_version}-${target_id}.tar.gz"
mdf_url="https://github.com/sa6mwa/libmdf/releases/download/v${mdf_version}/${mdf_archive}"
mdf_download="$archive_cache_root/$mdf_sha256/$mdf_archive"
mdf_lua_archive="libmdf-lua-${mdf_version}.tar.gz"
mdf_lua_url="https://github.com/sa6mwa/libmdf/releases/download/v${mdf_version}/${mdf_lua_archive}"
mdf_lua_sha256="fcff4bf1b4eb9165dadd76fa4dc9a2c4b8d8f409bb7200469f4b6e6177cf30e8"
mdf_lua_download="$archive_cache_root/$mdf_lua_sha256/$mdf_lua_archive"
mdf_lua_source_dir="$deps_root/share/libmdf-lua-source"
softline_version="0.9.0"
softline_archive="softline-${softline_version}-${target_id}.tar.gz"
softline_url="https://github.com/sa6mwa/softline/releases/download/v${softline_version}/${softline_archive}"
softline_download="$archive_cache_root/$softline_sha256/$softline_archive"
softline_lua_archive="softline-lua-${softline_version}.tar.gz"
softline_lua_payload=""
softline_lua_url="https://github.com/sa6mwa/softline/releases/download/v${softline_version}/${softline_lua_archive}"
softline_lua_sha256="98421d66740042aca38a18eb32245e736b49f6495595ffacd282c51c99b139fe"
softline_lua_download="$archive_cache_root/$softline_lua_sha256/$softline_lua_archive"
softline_lua_source_dir="$deps_root/share/softline-lua-source"
pid0_version="0.5.0"
pid0_header="libpid0-${pid0_version}.h"
pid0_header_gz="${pid0_header}.gz"
pid0_url="https://github.com/sa6mwa/libpid0/releases/download/v${pid0_version}/${pid0_header_gz}"
pid0_sha256="9647a9027e3f5086278cc628b4cfdf853e239a04c202b4f3577691202431c7e1"
pid0_download="$archive_cache_root/$pid0_sha256/$pid0_header_gz"
libxml2_version="2.15.4"
lua_version="5.5.1"
manifest_path="$deps_root/manifest.txt"

download_if_missing() {
  url=$1
  out=$2
  expected_sha256=$3
  archive_dir=$(dirname -- "$out")
  archive_name=$(basename -- "$out")
  mkdir -p "$archive_dir" "$archive_lock_root"
  (
    flock -w "${CPKT_DEPENDENCY_LOCK_TIMEOUT:-600}" 9
    if [ -f "$out" ]; then
      actual_sha256=$(sha256sum "$out" | awk '{print $1}')
      if [ "$actual_sha256" = "$expected_sha256" ]; then
        exit 0
      fi
      rm -f "$out"
    fi
    tmp="$archive_dir/.${archive_name}.$$"
    rm -f "$tmp"
    curl -L --fail --retry 3 --output "$tmp" "$url"
    actual_sha256=$(sha256sum "$tmp" | awk '{print $1}')
    if [ "$actual_sha256" != "$expected_sha256" ]; then
      rm -f "$tmp"
      echo "checksum mismatch for $archive_name" >&2
      echo "expected $expected_sha256" >&2
      echo "actual   $actual_sha256" >&2
      exit 1
    fi
    mv "$tmp" "$out"
  ) 9>"$archive_lock_root/$expected_sha256.lock"
}

download_if_missing "$system_url" "$system_download" "$system_sha256"
download_if_missing "$lockdc_url" "$lockdc_download" "$lockdc_sha256"
download_if_missing "$lockdc_lua_url" "$lockdc_lua_download" "$lockdc_lua_sha256"
download_if_missing "$lonejson_url" "$lonejson_download" "$lonejson_sha256"
download_if_missing "$lonejson_lua_url" "$lonejson_lua_download" "$lonejson_lua_sha256"
download_if_missing "$pslog_url" "$pslog_download" "$pslog_sha256"
download_if_missing "$pslog_lua_url" "$pslog_lua_download" "$pslog_lua_sha256"
download_if_missing "$cai_url" "$cai_download" "$cai_sha256"
download_if_missing "$cai_lua_url" "$cai_lua_download" "$cai_lua_sha256"
download_if_missing "$lql_url" "$lql_download" "$lql_sha256"
download_if_missing "$lql_lua_url" "$lql_lua_download" "$lql_lua_sha256"
download_if_missing "$mdf_url" "$mdf_download" "$mdf_sha256"
download_if_missing "$mdf_lua_url" "$mdf_lua_download" "$mdf_lua_sha256"
download_if_missing "$softline_url" "$softline_download" "$softline_sha256"
download_if_missing "$softline_lua_url" "$softline_lua_download" "$softline_lua_sha256"
case "$target_cmake_system_name" in
  Linux)
    pid0_enabled=1
    download_if_missing "$pid0_url" "$pid0_download" "$pid0_sha256"
    ;;
esac

actual_system_sha256=$(sha256sum "$system_download" | awk '{print $1}')
if [ "$actual_system_sha256" != "$system_sha256" ]; then
  echo "checksum mismatch for $system_archive" >&2
  echo "expected $system_sha256" >&2
  echo "actual   $actual_system_sha256" >&2
  exit 1
fi
actual_sha256=$(sha256sum "$lockdc_download" | awk '{print $1}')
if [ "$actual_sha256" != "$lockdc_sha256" ]; then
  echo "checksum mismatch for $lockdc_archive" >&2
  echo "expected $lockdc_sha256" >&2
  echo "actual   $actual_sha256" >&2
  exit 1
fi
actual_lockdc_lua_sha256=$(sha256sum "$lockdc_lua_download" | awk '{print $1}')
if [ "$actual_lockdc_lua_sha256" != "$lockdc_lua_sha256" ]; then
  echo "checksum mismatch for $lockdc_lua_archive" >&2
  echo "expected $lockdc_lua_sha256" >&2
  echo "actual   $actual_lockdc_lua_sha256" >&2
  exit 1
fi
actual_lonejson_sha256=$(sha256sum "$lonejson_download" | awk '{print $1}')
if [ "$actual_lonejson_sha256" != "$lonejson_sha256" ]; then
  echo "checksum mismatch for $lonejson_archive" >&2
  echo "expected $lonejson_sha256" >&2
  echo "actual   $actual_lonejson_sha256" >&2
  exit 1
fi
actual_lonejson_lua_sha256=$(sha256sum "$lonejson_lua_download" | awk '{print $1}')
if [ "$actual_lonejson_lua_sha256" != "$lonejson_lua_sha256" ]; then
  echo "checksum mismatch for $lonejson_lua_archive" >&2
  echo "expected $lonejson_lua_sha256" >&2
  echo "actual   $actual_lonejson_lua_sha256" >&2
  exit 1
fi
actual_pslog_sha256=$(sha256sum "$pslog_download" | awk '{print $1}')
if [ "$actual_pslog_sha256" != "$pslog_sha256" ]; then
  echo "checksum mismatch for $pslog_archive" >&2
  echo "expected $pslog_sha256" >&2
  echo "actual   $actual_pslog_sha256" >&2
  exit 1
fi
actual_pslog_lua_sha256=$(sha256sum "$pslog_lua_download" | awk '{print $1}')
if [ "$actual_pslog_lua_sha256" != "$pslog_lua_sha256" ]; then
  echo "checksum mismatch for $pslog_lua_archive" >&2
  echo "expected $pslog_lua_sha256" >&2
  echo "actual   $actual_pslog_lua_sha256" >&2
  exit 1
fi
actual_cai_sha256=$(sha256sum "$cai_download" | awk '{print $1}')
if [ "$actual_cai_sha256" != "$cai_sha256" ]; then
  echo "checksum mismatch for $cai_archive" >&2
  echo "expected $cai_sha256" >&2
  echo "actual   $actual_cai_sha256" >&2
  exit 1
fi
actual_cai_lua_sha256=$(sha256sum "$cai_lua_download" | awk '{print $1}')
if [ "$actual_cai_lua_sha256" != "$cai_lua_sha256" ]; then
  echo "checksum mismatch for $cai_lua_archive" >&2
  echo "expected $cai_lua_sha256" >&2
  echo "actual   $actual_cai_lua_sha256" >&2
  exit 1
fi
actual_lql_sha256=$(sha256sum "$lql_download" | awk '{print $1}')
if [ "$actual_lql_sha256" != "$lql_sha256" ]; then
  echo "checksum mismatch for $lql_archive" >&2
  echo "expected $lql_sha256" >&2
  echo "actual   $actual_lql_sha256" >&2
  exit 1
fi
actual_lql_lua_sha256=$(sha256sum "$lql_lua_download" | awk '{print $1}')
if [ "$actual_lql_lua_sha256" != "$lql_lua_sha256" ]; then
  echo "checksum mismatch for $lql_lua_archive" >&2
  echo "expected $lql_lua_sha256" >&2
  echo "actual   $actual_lql_lua_sha256" >&2
  exit 1
fi
actual_mdf_sha256=$(sha256sum "$mdf_download" | awk '{print $1}')
if [ "$actual_mdf_sha256" != "$mdf_sha256" ]; then
  echo "checksum mismatch for $mdf_archive" >&2
  echo "expected $mdf_sha256" >&2
  echo "actual   $actual_mdf_sha256" >&2
  exit 1
fi
actual_mdf_lua_sha256=$(sha256sum "$mdf_lua_download" | awk '{print $1}')
if [ "$actual_mdf_lua_sha256" != "$mdf_lua_sha256" ]; then
  echo "checksum mismatch for $mdf_lua_archive" >&2
  echo "expected $mdf_lua_sha256" >&2
  echo "actual   $actual_mdf_lua_sha256" >&2
  exit 1
fi
actual_softline_sha256=$(sha256sum "$softline_download" | awk '{print $1}')
if [ "$actual_softline_sha256" != "$softline_sha256" ]; then
  echo "checksum mismatch for $softline_archive" >&2
  echo "expected $softline_sha256" >&2
  echo "actual   $actual_softline_sha256" >&2
  exit 1
fi
actual_softline_lua_sha256=$(sha256sum "$softline_lua_download" | awk '{print $1}')
if [ "$actual_softline_lua_sha256" != "$softline_lua_sha256" ]; then
  echo "checksum mismatch for $softline_lua_archive" >&2
  echo "expected $softline_lua_sha256" >&2
  echo "actual   $actual_softline_lua_sha256" >&2
  exit 1
fi
if [ "$pid0_enabled" -eq 1 ]; then
  actual_pid0_sha256=$(sha256sum "$pid0_download" | awk '{print $1}')
  if [ "$actual_pid0_sha256" != "$pid0_sha256" ]; then
    echo "checksum mismatch for $pid0_header_gz" >&2
    echo "expected $pid0_sha256" >&2
    echo "actual   $actual_pid0_sha256" >&2
    exit 1
  fi
fi
rm -rf "$deps_root/include" "$deps_root/lib" "$deps_root/share"
mkdir -p "$deps_root"
tar -xzf "$system_download" -C "$deps_root" --strip-components 1
tar -xzf "$lockdc_download" -C "$deps_root" --strip-components 1
mkdir -p "$lockdc_lua_source_dir"
tar -xzf "$lockdc_lua_download" -C "$lockdc_lua_source_dir" --strip-components 1
patch_lockdc_lua_source
tar -xzf "$lonejson_download" -C "$deps_root" --strip-components 1
mkdir -p "$lonejson_source_dir"
tar -xzf "$lonejson_lua_download" -C "$lonejson_source_dir" --strip-components 1
tar -xzf "$pslog_download" -C "$deps_root" --strip-components 1
mkdir -p "$pslog_lua_source_dir"
tar -xzf "$pslog_lua_download" -C "$pslog_lua_source_dir" --strip-components 1
tar -xzf "$cai_download" -C "$deps_root" --strip-components 1
mkdir -p "$cai_lua_source_dir"
tar -xzf "$cai_lua_download" -C "$cai_lua_source_dir" --strip-components 1
tar -xzf "$lql_download" -C "$deps_root" --strip-components 1
mkdir -p "$lql_lua_source_dir"
tar -xzf "$lql_lua_download" -C "$lql_lua_source_dir" --strip-components 1
tar -xzf "$mdf_download" -C "$deps_root" --strip-components 1
mkdir -p "$mdf_lua_source_dir"
tar -xzf "$mdf_lua_download" -C "$mdf_lua_source_dir" --strip-components 1
tar -xzf "$softline_download" -C "$deps_root" --strip-components 1
mkdir -p "$softline_lua_source_dir"
tar -xzf "$softline_lua_download" -C "$softline_lua_source_dir" --strip-components 1
if [ "$pid0_enabled" -eq 1 ]; then
  gzip -dc "$pid0_download" > "$deps_root/include/$pid0_header"
fi

cat > "$manifest_path" <<EOF
preset=$preset
target_id=$target_id
system_archive=$system_archive
system_version=$system_version
system_sha256=$system_sha256
liblockdc_archive=$lockdc_archive
liblockdc_version=$lockdc_version
liblockdc_sha256=$lockdc_sha256
lockdc_lua_archive=$lockdc_lua_archive
lockdc_lua_payload=$lockdc_lua_payload
lockdc_lua_sha256=$lockdc_lua_sha256
lockdc_lua_patch=lc-version-if-version-helper
lonejson_archive=$lonejson_archive
lonejson_version=$lonejson_version
lonejson_sha256=$lonejson_sha256
lonejson_lua_archive=$lonejson_lua_archive
lonejson_lua_payload=$lonejson_lua_payload
lonejson_lua_sha256=$lonejson_lua_sha256
pslog_archive=$pslog_archive
pslog_version=$pslog_version
pslog_sha256=$pslog_sha256
pslog_source=libpslog-release
pslog_lua_archive=$pslog_lua_archive
pslog_lua_sha256=$pslog_lua_sha256
pslog_lua_source=lua-pslog-source-archive
cai_archive=$cai_archive
cai_version=$cai_version
cai_sha256=$cai_sha256
cai_lua_archive=$cai_lua_archive
cai_lua_payload=$cai_lua_payload
cai_lua_sha256=$cai_lua_sha256
cai_source=cai-release
cai_lua_source=cai-lua-source-archive
liblql_archive=$lql_archive
liblql_version=$lql_version
liblql_sha256=$lql_sha256
liblql_source=liblql-release
liblql_lua_archive=$lql_lua_archive
liblql_lua_sha256=$lql_lua_sha256
liblql_lua_source=liblql-lua-source-archive
libmdf_archive=$mdf_archive
libmdf_version=$mdf_version
libmdf_sha256=$mdf_sha256
libmdf_lua_archive=$mdf_lua_archive
libmdf_lua_sha256=$mdf_lua_sha256
libmdf_source=libmdf-release
libmdf_lua_source=libmdf-lua-source-archive
softline_archive=$softline_archive
softline_version=$softline_version
softline_sha256=$softline_sha256
softline_lua_archive=$softline_lua_archive
softline_lua_payload=$softline_lua_payload
softline_lua_sha256=$softline_lua_sha256
softline_source=softline-release
softline_lua_source=softline-lua-source-archive
lockdc_lua_source=lockdc-lua-source-archive
lonejson_lua_source=lonejson-lua-source-archive
curl_source=c.pkt.systems
openssl_source=c.pkt.systems
libssh2_source=c.pkt.systems
nghttp2_source=c.pkt.systems
zlib_source=c.pkt.systems
libpid0_enabled=$pid0_enabled
EOF
if [ "$pid0_enabled" -eq 1 ]; then
  cat >> "$manifest_path" <<EOF
libpid0_version=$pid0_version
libpid0_header=$pid0_header
libpid0_sha256=$pid0_sha256
EOF
fi
cat >> "$manifest_path" <<EOF
lua_version=$lua_version
lua_source=c.pkt.systems
lua_linkage=static+shared
lua_runtime_facade=cpkt-lua-runtime
lua_runtime_abi_version=0
audio_abi_version=0
sus_abi_version=0
opcua_abi_version=0
miniaudio_version=0.11.25
whisper_version=v1.9.4
open62541_version=1.5.8
libxml2_version=$libxml2_version
libxml2_source=c.pkt.systems
libxml2_iconv=enabled
libxml2_zlib=enabled
libxml2_catalog=default
libxml2_linkage=static+shared
EOF
