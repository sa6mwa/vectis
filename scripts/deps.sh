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
lql_lua_sha256="b440ce543586ebfc9aafd0e09a700126b9d62d85b8c34ae2ac19b0990db28438"
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
      system_sha256="0bbb1cbaf60b0a94fb5a6b3756123088b45e2bef9e38079038f22e3c07febb2e"
      lockdc_sha256="cc942533bd845b04885052047342f37e16ea898a33ef6f1aa5492b09ac69ebc2"
      lonejson_sha256="e04f80b907d92f7e38f825fbd339297e85372fc1ce110abb9a93715ee450ece3"
      pslog_sha256="7981ce7e60f6f1e144042e7a9192bb661472756ae34336fb0c2ed8316b31945f"
      cai_sha256="f73696bf4b79537e5ebe307a6e1ce5a28b455b557b6c6fbfcd92f9e37046b0c9"
      lql_sha256="a32b3ecc33b0634df23c630843b1c2c16a8a2caa947109a33bad20965e47a399"
      mdf_sha256="1ec76113c8326fa80ff85f15728bcd5128718a380a16fb3ff70ca169b297ba6d"
      softline_sha256="49641595f9c28bea3ba251240bc74506bab223b3abc8c366598ac54979d35313"
      target_cmake_system_processor="x86_64"
      ;;
    aarch64-linux-gnu)
      system_sha256="3fb1fdeb83bfd58da48a3319dfc2c6d35384265b2db2074b63216240bf0fe2ad"
      lockdc_sha256="864223aac2ab4cf6ecfda1464a9f673c24e343e375169ace7ee75f8989305390"
      lonejson_sha256="d7f9c700be6f9af7e46b18d59a0be14a42bd19644a30684b5a1135f96ee2daed"
      pslog_sha256="38bb08ca6646cf186925a724b61fb534fa49ec0d5e77ca95953dd7a5b18f76e1"
      cai_sha256="9acb14c98fd303d1551c81e50226e924c7f5aed471dbe49930e98fc28eb3984e"
      lql_sha256="d796c3b0574cb4137c22d4fce2ef04f24ded83199e6848ae0829dc10b276cc2f"
      mdf_sha256="d176ece0d8cf0e70268afdb6151ee17c4a03017dd1b0e453f06d318ce6cd02cd"
      softline_sha256="527b0d8cb1cc89b206f6584c21271d508ee7be79f5b4eb2fe7d2f8d2a70b17c2"
      target_cmake_system_processor="aarch64"
      ;;
    armhf-linux-gnu)
      system_sha256="18738e2d8e9661ebdcc0b54f4f292f0571d04218fceb1f281e051a50928d1694"
      lockdc_sha256="7652c2613c621e1943ee6e6cf8f72624fc14a71493bfb1473d1ca693c5388218"
      lonejson_sha256="3aeff1901078917a4430dc945c253cf4cec193311f35245b4ef1c62056d181c1"
      pslog_sha256="eff69fe9223cd2ad56572ad6acd768b560ac3e863e379c65367ad6338dbfffef"
      cai_sha256="da3cee2878e42d83d82f092044a83cb3a78cf0f0d8db765ddd4f83b400ec15dd"
      lql_sha256="fef9050c63f98f8a20d6afe9a428a172cb98a933b64221425e7eb759fb2284e6"
      mdf_sha256="c675573c46095ba62bb26164a1c07c29da8ecb1d6c173ebfa28a53341b091f3a"
      softline_sha256="5924f17d4989391c85cd732f62941284a1810c5eaaabbfe067767981a106b63d"
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
    system_sha256="e867e7d8649bba6d6c4bed254f3a666faa090f8ccb31a3eb10b1323b694f2f21"
    lockdc_sha256="4d2f18df71d0f17453493cecdcfab92d6606d67600cb51790ad629e0c50f1e57"
    lonejson_sha256="ca0811bd920f6cf59f82d45e04525b562bba238564e5c5b9a00aa18331b5a5ca"
    pslog_sha256="d05e59e8d88018a2e78e0941d2db211f3c08e4fd7539065ed2de79ce7e371055"
    cai_sha256="a0d4b3f277e8e20b53d03d54ec35e5b218b53bf2a22982bd3dbaf3be0907d115"
    lql_sha256="6a90dd82d5d12281a2afd05025a8eba179bc775cf015da9ee3359ab50f6adcfa"
    mdf_sha256="d0e48f2f3049d1d9c101314320eb7281d4ef983a081c8bf86ac56171f1879fba"
    softline_sha256="90210afbc1fb2308547d6e7394751805e2cf67112d34e4945ef0ae7f33bc5061"
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
    system_sha256="a915993c294e96c9a84b072bed45384c23f0058c9e18cd5caeba344aaa9b5d39"
    lockdc_sha256="6e5ce4fa402ed65e574528bf60e0816d8984e78935b44ff322889e41ecc06235"
    lonejson_sha256="813950b50620cfa48e01ae0c5b30ae338b79066e750c45cefeb9a86076466903"
    pslog_sha256="fce3c4f95b317563427437313ef2eb1987dc43973b0b0bf5169763d0a2705f69"
    cai_sha256="6de8fb83275aa1e773256982f9c0f9314a22fbb45816d9f8e5bacc71e80d083f"
    lql_sha256="b128e35e19267e6406c13831650be2cb5a20579cf142ac9beb9903705dc4ae5a"
    mdf_sha256="7556ae3bdb57d3ed4dfdb1468b29962a71cddca2b00b92935f67f57c97bec8b2"
    softline_sha256="336c239fd8885e49979aecb7f732f75edf5858e01e388f1e0d1a146163aefbc0"
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
    system_sha256="7f5365014ef2222cb95c08525c0b123afb30b4f220f4edcd669f354a9af4ccab"
    lockdc_sha256="5253c7efaa4fd0dece55a2d1121a320d7bc3e7ec64edd22ad624916b24c5ddc8"
    lonejson_sha256="37ba738c675b41c563b1b03ea322ad1e65dbe76749ebfd294809b50abafb2d32"
    pslog_sha256="19eeadacfb82b7eba4187b1fc405225bf85a8866ea81939e2eaa841a23d3785c"
    cai_sha256="5f4c97ce8a29477a59e5d818183bbc8cc1c2e664631d72d38df8c5700252f172"
    lql_sha256="af2b00cb119834bfeb5f80c410e02338e6a51bee058b80e2b70d7ede39c42ba0"
    mdf_sha256="6b1cfd9aae1fdb3426776cf3a10a960e0f8e5f0f2a82266b49c8300793a3ff9b"
    softline_sha256="08f90863ea68ed9ede69d9ff4fbfa878544e9fcb57a5ef300e7630a3ec1ebbcf"
    target_cc="cpkt-toolchains:$target_id"
    target_ar="cpkt-toolchains:$target_id"
    target_ranlib="cpkt-toolchains:$target_id"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="arm"
    ;;
  deps-arm64-apple-darwin)
    deps_root="$repo_root/.cache/deps/arm64-apple-darwin"
    target_id="arm64-apple-darwin"
    system_sha256="8bc25d47d30cb40b24eb5d07c2aad7850150fdea680eccadd1c819ce945901af"
    lockdc_sha256="b25989ab151f4d1798c5581a525de4c8e64fcafa1b5b411c9755333a6b47cc66"
    lonejson_sha256="b351df4221e16d62b7b86940a6a6a6a4d38fffb850b2d118fca2f9f5a9bb5488"
    pslog_sha256="ff5d2106bcbc5ea5bce8dfdbca54d21650f350e50fd214a4b52ac65b4f834073"
    cai_sha256="cfc4cc6142e476c0e00a1435ffaefa1484e0d502e794c6c49dd7270f27844a81"
    lql_sha256="2e01c19a9ee0a12bc8e6a1411d8f048a9e2f57fe336b28332ec0fc6307665725"
    mdf_sha256="99504a42eec7ee21dedbda724211e43673fc7f134926111ed15b21ea6e19d49b"
    softline_sha256="f4f6777adcfc36d0f0eef735c9e838e20c501305af3756ceeecf9f0a7655f2ef"
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
libpid0_version=0.4.2
libpid0_sha256=907fb7f084d192da1c6d92d26b1b6cd93bb4dafe36737856696f47de906dd5e8
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

system_version="0.9.0"
system_archive="c.pkt.systems-${system_version}-${target_id}.tar.gz"
system_url="https://github.com/sa6mwa/c.pkt.systems/releases/download/v${system_version}/${system_archive}"
system_download="$archive_cache_root/$system_sha256/$system_archive"
lockdc_version="0.15.0"
lockdc_archive="liblockdc-${lockdc_version}-${target_id}.tar.gz"
lockdc_url="https://github.com/sa6mwa/liblockdc/releases/download/v${lockdc_version}/${lockdc_archive}"
lockdc_download="$archive_cache_root/$lockdc_sha256/$lockdc_archive"
lockdc_lua_archive="liblockdc-lua-${lockdc_version}.tar.gz"
lockdc_lua_payload=""
lockdc_lua_url="https://github.com/sa6mwa/liblockdc/releases/download/v${lockdc_version}/${lockdc_lua_archive}"
lockdc_lua_sha256="d65a52bfce6d7c0035e0add89ae517bd02d9b61f92db54e70c8ef6c2691ec744"
lockdc_lua_download="$archive_cache_root/$lockdc_lua_sha256/$lockdc_lua_archive"
lockdc_lua_source_dir="$deps_root/share/lockdc-source"
lonejson_version="0.42.0"
lonejson_archive="liblonejson-${lonejson_version}-${target_id}.tar.gz"
lonejson_url="https://github.com/sa6mwa/lonejson/releases/download/v${lonejson_version}/${lonejson_archive}"
lonejson_download="$archive_cache_root/$lonejson_sha256/$lonejson_archive"
lonejson_lua_archive="lonejson-lua-${lonejson_version}.tar.gz"
lonejson_lua_payload=""
lonejson_lua_url="https://github.com/sa6mwa/lonejson/releases/download/v${lonejson_version}/${lonejson_lua_archive}"
lonejson_lua_sha256="8d6d25eb6cbd46eafc7e2ee555d8bde1b0dee435a97a3882b93a7533ba6b2ee4"
lonejson_lua_download="$archive_cache_root/$lonejson_lua_sha256/$lonejson_lua_archive"
lonejson_source_dir="$deps_root/share/lonejson-source"
pslog_version="0.9.0"
pslog_archive="libpslog-${pslog_version}-${target_id}.tar.gz"
pslog_url="https://github.com/sa6mwa/libpslog/releases/download/v${pslog_version}/${pslog_archive}"
pslog_download="$archive_cache_root/$pslog_sha256/$pslog_archive"
pslog_lua_archive="lua-pslog-${pslog_version}.tar.gz"
pslog_lua_url="https://github.com/sa6mwa/libpslog/releases/download/v${pslog_version}/${pslog_lua_archive}"
pslog_lua_sha256="5a73e94395ec484ff7c404651a892297dfd1e863bc361490fe95f2b7f9c5db71"
pslog_lua_download="$archive_cache_root/$pslog_lua_sha256/$pslog_lua_archive"
pslog_lua_source_dir="$deps_root/share/pslog-lua-source"
cai_version="0.5.0"
cai_archive="cai-${cai_version}-${target_id}.tar.gz"
cai_url="https://github.com/sa6mwa/cai/releases/download/v${cai_version}/${cai_archive}"
cai_download="$archive_cache_root/$cai_sha256/$cai_archive"
cai_lua_archive="cai-lua-${cai_version}.tar.gz"
cai_lua_payload=""
cai_lua_url="https://github.com/sa6mwa/cai/releases/download/v${cai_version}/${cai_lua_archive}"
cai_lua_sha256="c4ce287de641fcc3da6afab40800bab150b21859f98ea6d4e9b83e697fc6e846"
cai_lua_download="$archive_cache_root/$cai_lua_sha256/$cai_lua_archive"
cai_lua_source_dir="$deps_root/share/cai-lua-source"
lql_version="0.2.0"
lql_archive="liblql-${lql_version}-${target_id}.tar.gz"
lql_url="https://github.com/sa6mwa/liblql/releases/download/v${lql_version}/${lql_archive}"
lql_download="$archive_cache_root/$lql_sha256/$lql_archive"
lql_lua_archive="liblql-lua-${lql_version}.tar.gz"
lql_lua_url="https://github.com/sa6mwa/liblql/releases/download/v${lql_version}/${lql_lua_archive}"
lql_lua_download="$archive_cache_root/$lql_lua_sha256/$lql_lua_archive"
lql_lua_source_dir="$deps_root/share/liblql-lua-source"
mdf_version="0.8.0"
mdf_archive="libmdf-${mdf_version}-${target_id}.tar.gz"
mdf_url="https://github.com/sa6mwa/libmdf/releases/download/v${mdf_version}/${mdf_archive}"
mdf_download="$archive_cache_root/$mdf_sha256/$mdf_archive"
mdf_lua_archive="libmdf-lua-${mdf_version}.tar.gz"
mdf_lua_url="https://github.com/sa6mwa/libmdf/releases/download/v${mdf_version}/${mdf_lua_archive}"
mdf_lua_sha256="0248982263bbd0df348a3e2c40e7a2c369d92ee38972700ede9f883f7e6b1af5"
mdf_lua_download="$archive_cache_root/$mdf_lua_sha256/$mdf_lua_archive"
mdf_lua_source_dir="$deps_root/share/libmdf-lua-source"
softline_version="0.4.0"
softline_archive="softline-${softline_version}-${target_id}.tar.gz"
softline_url="https://github.com/sa6mwa/softline/releases/download/v${softline_version}/${softline_archive}"
softline_download="$archive_cache_root/$softline_sha256/$softline_archive"
softline_lua_archive="softline-lua-${softline_version}.tar.gz"
softline_lua_payload=""
softline_lua_url="https://github.com/sa6mwa/softline/releases/download/v${softline_version}/${softline_lua_archive}"
softline_lua_sha256="2e9e6066ffd5d3b63dfef9166769ca2e82ab348f23243dca938878001e8bab11"
softline_lua_download="$archive_cache_root/$softline_lua_sha256/$softline_lua_archive"
softline_lua_source_dir="$deps_root/share/softline-lua-source"
pid0_version="0.4.2"
pid0_header="libpid0-${pid0_version}.h"
pid0_header_gz="${pid0_header}.gz"
pid0_url="https://github.com/sa6mwa/libpid0/releases/download/v${pid0_version}/${pid0_header_gz}"
pid0_sha256="907fb7f084d192da1c6d92d26b1b6cd93bb4dafe36737856696f47de906dd5e8"
pid0_download="$archive_cache_root/$pid0_sha256/$pid0_header_gz"
libxml2_version="2.15.3"
lua_version="5.5.0"
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
case "$target_cmake_system_name" in
  Linux)
    if [ -e "$deps_root/lib/liblonejson.so.$lonejson_version" ] && [ ! -e "$deps_root/lib/liblonejson.so.25" ]; then
      ln -s "liblonejson.so.$lonejson_version" "$deps_root/lib/liblonejson.so.25"
    fi
    ;;
esac
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
whisper_version=v1.9.1
open62541_version=1.5.4
libxml2_version=$libxml2_version
libxml2_source=c.pkt.systems
libxml2_iconv=enabled
libxml2_zlib=enabled
libxml2_catalog=default
libxml2_linkage=static+shared
EOF
