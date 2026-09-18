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
      system_sha256="fb64caa3cad66e01669705412cd88cb4267025ca0a083fa936fe25786011391d"
      lockdc_sha256="b5060d5a82a5aa5e97bdad2376828a559ae40d9da5d93c904eb85b8a1b76d286"
      lonejson_sha256="520e045c7bfe13396b9dd2ef664d2aaf723ff3173ac02e0f0fe08bcf96f14fa2"
      pslog_sha256="db4089205cd674ec65540ea7a2388a827a8db5dd7c841db39d4beb8d9eff3d26"
      cai_sha256="f7f78d9f847f1265267bbc33a6d6cb2058602d82967357ab561f36ebb1d0daf8"
      lql_sha256="a32b3ecc33b0634df23c630843b1c2c16a8a2caa947109a33bad20965e47a399"
      mdf_sha256="1ec76113c8326fa80ff85f15728bcd5128718a380a16fb3ff70ca169b297ba6d"
      softline_sha256="b1e4f2fb8ad4d8072acf1d72be31ec23066d8ccbf66a6fe10e15b7243ac8ac2f"
      target_cmake_system_processor="x86_64"
      ;;
    aarch64-linux-gnu)
      system_sha256="111e8f5215728ae8c784c59de35fc33822a7f72bc1c48d807013b678077d6307"
      lockdc_sha256="3bb0df50d2efd07f4bad8e497975913c1e4613fc30757eca66c516c5a6fdf2a3"
      lonejson_sha256="beacf0b574fc90478d37cd624c70edeac868a3d46241becbc82652ad430fd79e"
      pslog_sha256="3d08b7a8e175805c9441edd67cfad29dda3593eb03273b1011a70e959ddc2426"
      cai_sha256="4fa244a597378ee097a10ee5a472c8d07df5208e6a570b6756ccae40e96abb18"
      lql_sha256="d796c3b0574cb4137c22d4fce2ef04f24ded83199e6848ae0829dc10b276cc2f"
      mdf_sha256="d176ece0d8cf0e70268afdb6151ee17c4a03017dd1b0e453f06d318ce6cd02cd"
      softline_sha256="4345b688cb490080d48c6055cee11517991122af9179b42a3da9ef160ceea968"
      target_cmake_system_processor="aarch64"
      ;;
    armhf-linux-gnu)
      system_sha256="df5e953677217c87314dd01514227cbdbbd2fb72115083b5747606f37f00d060"
      lockdc_sha256="3f83eff6c90ae8d96fcb084565576f4744cf23dea1b0c78eaed2c1de00c00f96"
      lonejson_sha256="95595747b861440c189fc473cdaefec2529df15bfd6772b627c637c93bacf3a9"
      pslog_sha256="5b0207766882dd1cb913e49c4c5ec6e9643a38a241045457a0e64d415a8295cd"
      cai_sha256="f98666e5b0eb60d6a61bfa0f8ca10b018e4bc32a267cd7fc2a1532d9e256a7dd"
      lql_sha256="fef9050c63f98f8a20d6afe9a428a172cb98a933b64221425e7eb759fb2284e6"
      mdf_sha256="c675573c46095ba62bb26164a1c07c29da8ecb1d6c173ebfa28a53341b091f3a"
      softline_sha256="b0b3ab58d5d60611f0fefb180a9d0c592f9cee2d63020e2c15b66408b06dad83"
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
    system_sha256="668977f4a14875d4ee2acec6f3d630ce96b57786d3ab3242c6ee3d10b5e63c87"
    lockdc_sha256="457fe622663f6bd89232217ab0a2ad17bc71eac4237d9fae820cb221bdcc3608"
    lonejson_sha256="f830d9e848663fb780bcabe58fcd0e61c93ff2725484d8133c3ab3318ebd8a59"
    pslog_sha256="b9c1f212df866e03a2d55bf95a1294f678b3d4765ec09281d7294653225ae424"
    cai_sha256="5a9a857137e1fa8fd1679fe8d6c6b0d84f69bdd905deeb2fdcbf35db1663872c"
    lql_sha256="6a90dd82d5d12281a2afd05025a8eba179bc775cf015da9ee3359ab50f6adcfa"
    mdf_sha256="d0e48f2f3049d1d9c101314320eb7281d4ef983a081c8bf86ac56171f1879fba"
    softline_sha256="e164bd5b3762158f27fc74393f1e76be55aa93ae82d50ac27ec343cc5b249707"
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
    system_sha256="39883280a4e37a03c7ba952421cfdd926e79b49b635200c1fcb783738200c541"
    lockdc_sha256="eed2a0528aed92c79b2528800261b4764f00ab62fb4e404dfc3fe93f134bc6fc"
    lonejson_sha256="5e57b91810389dc61111ce77ce4c17b6704884be1ee21c69d96e1e594cb9ce8b"
    pslog_sha256="80a5374714479311b3f1f232f8ef658f693ed03d7c1b21bc5d913b8b9df4fc10"
    cai_sha256="0682edace79a151bde43b040933d10d2ead4659aba02bc1d7daf0e06b36c4861"
    lql_sha256="b128e35e19267e6406c13831650be2cb5a20579cf142ac9beb9903705dc4ae5a"
    mdf_sha256="7556ae3bdb57d3ed4dfdb1468b29962a71cddca2b00b92935f67f57c97bec8b2"
    softline_sha256="d2c46062f8417e706224fffee0ece68aab34602acd2c3f410a2d387be87193e2"
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
    system_sha256="d78e8e3de15d497c9048cfd3e64db815ee7e7d707ec2ba66e094af13f979ebdc"
    lockdc_sha256="35e19b6355412d075d7e346d9ebb1d35085e554869eb3596bdb5cb84bb383528"
    lonejson_sha256="22a8507bd441e12c00e813a16c286478bd8c797a31028e406473e45de6712f5a"
    pslog_sha256="daa40aec6622f7c9cfb650954ef86ac0b22ed718277d03d39f27114d28710889"
    cai_sha256="90e8982c3accf4accc338e4c6ec9a53b1d80d6f62e4d6d50f5c32de4599d9f52"
    lql_sha256="af2b00cb119834bfeb5f80c410e02338e6a51bee058b80e2b70d7ede39c42ba0"
    mdf_sha256="6b1cfd9aae1fdb3426776cf3a10a960e0f8e5f0f2a82266b49c8300793a3ff9b"
    softline_sha256="5323ea3e8f4f1693333331ce710f9a82c2ecd7b485cc960aa10651eae981a988"
    target_cc="cpkt-toolchains:$target_id"
    target_ar="cpkt-toolchains:$target_id"
    target_ranlib="cpkt-toolchains:$target_id"
    target_cmake_system_name="Linux"
    target_cmake_system_processor="arm"
    ;;
  deps-arm64-apple-darwin)
    deps_root="$repo_root/.cache/deps/arm64-apple-darwin"
    target_id="arm64-apple-darwin"
    system_sha256="f63f0e6e847108b287726dbd8d2566ee9255d2c744d8cfcf3b192a4ca791de77"
    lockdc_sha256="770ea5fc385cc86be9fcc38db94e3df8102c720c7b6c0f7a9542e382ddbaf01b"
    lonejson_sha256="2e5e349995e5bf6d84004ea0b829d51ae2c2ca90229a98104a640f155dd3c7ab"
    pslog_sha256="64a742fb493785ac234901eefa678e73f04441146d53009551078431cdc68e20"
    cai_sha256="1921498c951abd7aa093760f3307563a9ac6e98f22fffc8ec7f81d7649de47b6"
    lql_sha256="2e01c19a9ee0a12bc8e6a1411d8f048a9e2f57fe336b28332ec0fc6307665725"
    mdf_sha256="99504a42eec7ee21dedbda724211e43673fc7f134926111ed15b21ea6e19d49b"
    softline_sha256="2217093e8ca172bab1f9721557e072ae997ca41ae6d76abd038ad14cbacef03c"
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

system_version="0.10.0"
system_archive="c.pkt.systems-${system_version}-${target_id}.tar.gz"
system_url="https://github.com/sa6mwa/c.pkt.systems/releases/download/v${system_version}/${system_archive}"
system_download="$archive_cache_root/$system_sha256/$system_archive"
lockdc_version="0.17.0"
lockdc_archive="liblockdc-${lockdc_version}-${target_id}.tar.gz"
lockdc_url="https://github.com/sa6mwa/liblockdc/releases/download/v${lockdc_version}/${lockdc_archive}"
lockdc_download="$archive_cache_root/$lockdc_sha256/$lockdc_archive"
lockdc_lua_archive="liblockdc-lua-${lockdc_version}.tar.gz"
lockdc_lua_payload=""
lockdc_lua_url="https://github.com/sa6mwa/liblockdc/releases/download/v${lockdc_version}/${lockdc_lua_archive}"
lockdc_lua_sha256="b5d7ae8f2400101e2550b1232d71e80fd028d993cf617f05e43c5994122e32b7"
lockdc_lua_download="$archive_cache_root/$lockdc_lua_sha256/$lockdc_lua_archive"
lockdc_lua_source_dir="$deps_root/share/lockdc-source"
lonejson_version="0.43.0"
lonejson_archive="liblonejson-${lonejson_version}-${target_id}.tar.gz"
lonejson_url="https://github.com/sa6mwa/lonejson/releases/download/v${lonejson_version}/${lonejson_archive}"
lonejson_download="$archive_cache_root/$lonejson_sha256/$lonejson_archive"
lonejson_lua_archive="lonejson-lua-${lonejson_version}.tar.gz"
lonejson_lua_payload=""
lonejson_lua_url="https://github.com/sa6mwa/lonejson/releases/download/v${lonejson_version}/${lonejson_lua_archive}"
lonejson_lua_sha256="ae2e6889f00429f6f537dd6e7589f019cc5e4bd489add4ee8527413dfd7f693d"
lonejson_lua_download="$archive_cache_root/$lonejson_lua_sha256/$lonejson_lua_archive"
lonejson_source_dir="$deps_root/share/lonejson-source"
pslog_version="0.10.0"
pslog_archive="libpslog-${pslog_version}-${target_id}.tar.gz"
pslog_url="https://github.com/sa6mwa/libpslog/releases/download/v${pslog_version}/${pslog_archive}"
pslog_download="$archive_cache_root/$pslog_sha256/$pslog_archive"
pslog_lua_archive="lua-pslog-${pslog_version}.tar.gz"
pslog_lua_url="https://github.com/sa6mwa/libpslog/releases/download/v${pslog_version}/${pslog_lua_archive}"
pslog_lua_sha256="a58a8601023efdb723f3aae51af7e12a169951e51081ea8029a45782684b27be"
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
softline_version="0.6.0"
softline_archive="softline-${softline_version}-${target_id}.tar.gz"
softline_url="https://github.com/sa6mwa/softline/releases/download/v${softline_version}/${softline_archive}"
softline_download="$archive_cache_root/$softline_sha256/$softline_archive"
softline_lua_archive="softline-lua-${softline_version}.tar.gz"
softline_lua_payload=""
softline_lua_url="https://github.com/sa6mwa/softline/releases/download/v${softline_version}/${softline_lua_archive}"
softline_lua_sha256="f8cc7ea7c3dbf3b336c55be10972435a03b5783d8773b8506e928806ebefaa36"
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
